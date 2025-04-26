#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <semaphore.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>
#include <sys/select.h> 
#include <sys/time.h>
#include <errno.h>

//#define CONFIG_FILE "../config/config.txt"
#define CONFIG_FILE "config/config.txt" 
/*esta ruta funciona desde bin, pero no desde /BANCO */
//#define LOG_FILE "../data/transacciones.log"
#define LOG_FILE "data/transacciones.log"
#define MAX_USUARIOS_SIMULTANEOS 10
#define FIFO_BASE_PATH "/tmp/banco_fifo_"

typedef struct {
    double limite_retiro;
    int limite_transferencia;
    int umbral_retiros;
    int umbral_transferencias;
    int num_hilos;
    char archivo_cuentas[256];
    char archivo_log[256];
} Config;

Config config;
int continuar_ejecucion = 1;  // Flag para controlar el bucle principal

// Estructura para mantener información de usuarios activos
typedef struct {
    pid_t pid;               // PID del proceso usuario
    int cuenta;              // Número de cuenta
    int fifo_lectura_fd;     // Descriptor para leer del usuario
    int fifo_escritura_fd;   // Descriptor para escribir al usuario
    char fifo_lectura[100];  // Ruta al FIFO para leer del usuario
    char fifo_escritura[100]; // Ruta al FIFO para escribir al usuario
} InfoUsuario;

InfoUsuario usuarios[MAX_USUARIOS_SIMULTANEOS];

/* Función para manejar señales y terminar adecuadamente */
void manejador_senales(int sig) {
    printf("\nSeñal recibida (%d). Terminando proceso banco...\n", sig);
    continuar_ejecucion = 0;
}

/* Función para leer la configuración desde el archivo.
   Se espera que cada línea siga el formato clave=valor sin espacios extra. */
void leer_configuracion(const char *filename, Config *cfg) {
    FILE *file = fopen(filename, "r");
    if (file == NULL) {
        perror("Error al abrir el archivo de configuración");
        exit(EXIT_FAILURE);
    }
    
    char line[256];
    while (fgets(line, sizeof(line), file)) {
        // Eliminar salto de línea
        line[strcspn(line, "\n")] = 0;
        if (strncmp(line, "LIMITE_RETIRO=", 14) == 0) {
            cfg->limite_retiro = atoi(line + 14);
        } else if (strncmp(line, "LIMITE_TRANSFERENCIA=", 21) == 0) {
            cfg->limite_transferencia = atoi(line + 21);
        } else if (strncmp(line, "UMBRAL_RETIROS=", 15) == 0) {
            cfg->umbral_retiros = atoi(line + 15);
        } else if (strncmp(line, "UMBRAL_TRANSFERENCIAS=", 22) == 0) {
            cfg->umbral_transferencias = atoi(line + 22);
        } else if (strncmp(line, "NUM_HILOS=", 10) == 0) {
            cfg->num_hilos = atoi(line + 10);
        } else if (strncmp(line, "ARCHIVO_CUENTAS=", 16) == 0) {
            sscanf(line + 16, "%s", cfg->archivo_cuentas);
        } else if (strncmp(line, "ARCHIVO_LOG=", 12) == 0) {
            sscanf(line + 12, "%s", cfg->archivo_log);
        }
    }
    fclose(file);
}

// Función para crear un FIFO con manejo de errores
int crear_fifo(const char *path) {
    if (mkfifo(path, 0666) == -1) {
        if (errno != EEXIST) {
            perror("Error al crear FIFO");
            return -1;
        }
    }
    return 0;
}

// Función para limpiar recursos de un usuario
void limpiar_recursos_usuario(int idx) {
    if (idx < 0 || idx >= MAX_USUARIOS_SIMULTANEOS) return;
    
    if (usuarios[idx].fifo_lectura_fd > 0) {
        close(usuarios[idx].fifo_lectura_fd);
        usuarios[idx].fifo_lectura_fd = 0;
    }
    
    if (usuarios[idx].fifo_escritura_fd > 0) {
        close(usuarios[idx].fifo_escritura_fd);
        usuarios[idx].fifo_escritura_fd = 0;
    }
    
    // Eliminar los FIFOs
    if (strlen(usuarios[idx].fifo_lectura) > 0) {
        unlink(usuarios[idx].fifo_lectura);
        usuarios[idx].fifo_lectura[0] = '\0';
    }
    
    if (strlen(usuarios[idx].fifo_escritura) > 0) {
        unlink(usuarios[idx].fifo_escritura);
        usuarios[idx].fifo_escritura[0] = '\0';
    }
    
    usuarios[idx].pid = 0;
    usuarios[idx].cuenta = 0;
}

int main() {
    // Inicializar array de usuarios
    for (int i = 0; i < MAX_USUARIOS_SIMULTANEOS; i++) {
        usuarios[i].pid = 0;
        usuarios[i].cuenta = 0;
        usuarios[i].fifo_lectura_fd = 0;
        usuarios[i].fifo_escritura_fd = 0;
        usuarios[i].fifo_lectura[0] = '\0';
        usuarios[i].fifo_escritura[0] = '\0';
    }

    // Leer el fichero de configuración.
    leer_configuracion(CONFIG_FILE, &config);
    

    // Configuración de manejadores de señales para terminación adecuada
    signal(SIGINT, manejador_senales);
    signal(SIGTERM, manejador_senales);

    // Crear un semáforo nombrado para controlar el acceso al archivo de cuentas.
    sem_t *sem = sem_open("/cuentas_semaphore", O_CREAT, 0644, 1);
    if (sem == SEM_FAILED) {
        perror("Error al crear el semáforo");
        exit(EXIT_FAILURE);
    }

    // Abrir el archivo de log.
    const char *log_filename = strlen(config.archivo_log) > 0 ? config.archivo_log : LOG_FILE;
    FILE *log_file = fopen(log_filename, "a");
    if (log_file == NULL) {
        perror("Error al abrir el archivo de log");
        exit(EXIT_FAILURE);
    }

    printf("Banco iniciado. Esperando conexiones de usuario...\n");
    printf("Presione Ctrl+C para terminar.\n\n");

    // Variables para el manejo no bloqueante de la entrada
    fd_set read_fds;
    struct timeval tv;
    int stdin_fd = fileno(stdin); // Descriptor de archivo para stdin
    fcntl(stdin_fd, F_SETFL, fcntl(stdin_fd, F_GETFL) | O_NONBLOCK);

    // Bucle principal mejorado
    while (continuar_ejecucion) {
        // 1. Comprobar si hay nuevas conexiones de usuario (no bloqueante)
        FD_ZERO(&read_fds);
        FD_SET(stdin_fd, &read_fds);
        tv.tv_sec = 0;
        tv.tv_usec = 1000; // 1ms timeout
        
        // Comprobar si hay entrada disponible (nuevo número de cuenta)
        if (select(stdin_fd + 1, &read_fds, NULL, NULL, &tv) > 0) {
            int cuenta_usuario;
            int slot_disponible = -1;

            // Buscar un slot disponible para un nuevo usuario
            for (int i = 0; i < MAX_USUARIOS_SIMULTANEOS; i++) {
                if (usuarios[i].pid == 0) {
                    slot_disponible = i;
                    break;
                }
            }

            if (slot_disponible == -1) {
                printf("Se ha alcanzado el límite de usuarios simultáneos. Espere...\n");
                // Consumir la entrada
                int tmp;
                scanf("%d", &tmp);
                continue;
            }

            // Leer el número de cuenta
            printf("Ingrese el número de cuenta (o 0 para salir): ");
            if (scanf("%d", &cuenta_usuario) != 1) {
                printf("Entrada inválida. Intente de nuevo.\n");
                while (getchar() != '\n'); // Limpiar buffer
                continue;
            }

            if (cuenta_usuario == 0) {
                printf("Solicitud de cierre recibida.\n");
                continuar_ejecucion = 0;
                continue;
            }

            // Crear FIFOs ANTES de forkear
            char fifo_to_usuario[100], fifo_from_usuario[100];
            sprintf(fifo_to_usuario, "%s%d_to_user", FIFO_BASE_PATH, slot_disponible);
            sprintf(fifo_from_usuario, "%s%d_from_user", FIFO_BASE_PATH, slot_disponible);
            
            // Guardar nombres en struct
            strcpy(usuarios[slot_disponible].fifo_escritura, fifo_to_usuario);
            strcpy(usuarios[slot_disponible].fifo_lectura, fifo_from_usuario);
            
            if (crear_fifo(fifo_to_usuario) < 0 || crear_fifo(fifo_from_usuario) < 0) {
                fprintf(stderr, "Error al crear FIFOs para el usuario %d\n", cuenta_usuario);
                // Limpiar FIFOs si uno falló
                unlink(fifo_to_usuario);
                unlink(fifo_from_usuario);
                continue;
            }
            
            // Abrir extremo de LECTURA del banco ANTES de forkear
            usuarios[slot_disponible].fifo_lectura_fd = open(fifo_from_usuario, O_RDWR | O_NONBLOCK);
            if (usuarios[slot_disponible].fifo_lectura_fd < 0) {
                perror("Error al abrir FIFO para lectura (banco)");
                unlink(fifo_to_usuario);
                unlink(fifo_from_usuario);
                continue;
            }
            
            pid_t pid = fork();
            if (pid < 0) {
                perror("Error al crear el proceso hijo");
                close(usuarios[slot_disponible].fifo_lectura_fd); // Cerrar el que abrimos
                unlink(fifo_to_usuario);
                unlink(fifo_from_usuario);
                usuarios[slot_disponible].pid = 0;
                continue;
            } else if (pid == 0) {
                // ***** PROCESO HIJO *****
                
                // Cerrar descriptores innecesarios heredados
                close(usuarios[slot_disponible].fifo_lectura_fd);
                
                char cuenta_str[20];
                sprintf(cuenta_str, "%d", cuenta_usuario);
                char titulo_ventana[64];
                sprintf(titulo_ventana, "Usuario Banco - Cuenta %d", cuenta_usuario);
                
                // SOLUCIÓN: Usar execvp en lugar de system para mantener el PID correcto
                // Construir argumentos para execvp
                char *args[] = {
                    "xterm",
                    "-T", titulo_ventana,
                    "-e", 
                    "bin/usuario", //modificado
                    cuenta_str, 
                    fifo_from_usuario, 
                    fifo_to_usuario,
                    NULL
                };

                execvp("xterm", args);
                
                // Si xterm falla, intentar con gnome-terminal
                fprintf(stderr, "[Hijo %d] Falló execvp con xterm: %s. Intentando gnome-terminal...\n", 
                        getpid(), strerror(errno));
                
                // Construir argumentos para gnome-terminal
                char *args_gnome[] = {
                    "gnome-terminal",
                    "--",
                    "./usuario", 
                    cuenta_str, 
                    fifo_from_usuario, 
                    fifo_to_usuario,
                    NULL
                };
                
                execvp("gnome-terminal", args_gnome);
                
                // Si ambos fallan, intentar ejecutar directamente
                fprintf(stderr, "[Hijo %d] Falló execvp con gnome-terminal: %s. Ejecutando usuario directamente...\n", 
                        getpid(), strerror(errno));
                
                // Construir argumentos para usuario directo
                char *args_usuario[] = {
                    "./usuario", 
                    cuenta_str, 
                    fifo_from_usuario, 
                    fifo_to_usuario,
                    NULL
                };
                
                execvp("./usuario", args_usuario);
                
                // Si todo falla, mostrar error y salir
                perror("[Hijo] Error crítico: No se pudo ejecutar ninguna terminal ni el usuario");
                exit(EXIT_FAILURE);
                
            } else {
                // ***** PROCESO PADRE (BANCO) *****
                usuarios[slot_disponible].pid = pid;
                usuarios[slot_disponible].cuenta = cuenta_usuario;
                
                printf("Proceso usuario lanzado con PID: %d para cuenta %d\n", pid, cuenta_usuario);
                
                // Ahora, intentar abrir el FIFO de escritura del banco sin O_NONBLOCK para operaciones críticas
                usuarios[slot_disponible].fifo_escritura_fd = open(fifo_to_usuario, O_RDWR);
                if (usuarios[slot_disponible].fifo_escritura_fd < 0) {
                    perror("Error al abrir FIFO para escritura (banco)");
                    close(usuarios[slot_disponible].fifo_lectura_fd);
                    kill(pid, SIGTERM);
                    waitpid(pid, NULL, 0);
                    limpiar_recursos_usuario(slot_disponible);
                    continue;
                }
                
                // Cambiar el FIFO de lectura también a modo bloqueante para operaciones críticas
                close(usuarios[slot_disponible].fifo_lectura_fd);
                usuarios[slot_disponible].fifo_lectura_fd = open(fifo_from_usuario, O_RDWR);
                if (usuarios[slot_disponible].fifo_lectura_fd < 0) {
                    perror("Error al reabrir FIFO para lectura (banco)");
                    close(usuarios[slot_disponible].fifo_escritura_fd);
                    kill(pid, SIGTERM);
                    waitpid(pid, NULL, 0);
                    limpiar_recursos_usuario(slot_disponible);
                    continue;
                }
                
                // Configurar el modo no-bloqueante después de establecer la conexión
                fcntl(usuarios[slot_disponible].fifo_lectura_fd, F_SETFL, 
                      fcntl(usuarios[slot_disponible].fifo_lectura_fd, F_GETFL) | O_NONBLOCK);
                
                printf("Comunicación establecida con usuario cuenta %d (PID: %d)\n", cuenta_usuario, pid);
                fprintf(log_file, "Usuario conectado: Cuenta %d (PID: %d)\n", cuenta_usuario, pid);
                fflush(log_file);
                
                // Esperar un poco para asegurar que el usuario esté listo
                usleep(200000); // 200ms
                
                // Enviar mensaje de bienvenida
                char mensaje_bienvenida[256];
                sprintf(mensaje_bienvenida, "Bienvenido usuario con cuenta %d. Conexión establecida con el banco.\n", cuenta_usuario);
                if (write(usuarios[slot_disponible].fifo_escritura_fd, mensaje_bienvenida, strlen(mensaje_bienvenida)) < 0) {
                    perror("Error al enviar mensaje de bienvenida");
                }
            }
        }

        // 2. Verificar si hay procesos hijo que han terminado
        for (int i = 0; i < MAX_USUARIOS_SIMULTANEOS; i++) {
            if (usuarios[i].pid > 0) {
                int status;
                pid_t result = waitpid(usuarios[i].pid, &status, WNOHANG);
                
                if (result == usuarios[i].pid) { // El hijo específico terminó
                    printf("Usuario (Terminal PID: %d) desconectado.\n", usuarios[i].pid);
                    if (WIFEXITED(status)) {
                        printf("  Estado de salida: %d\n", WEXITSTATUS(status));
                    } else if (WIFSIGNALED(status)) {
                        printf("  Terminado por señal: %d\n", WTERMSIG(status));
                    }
                    fprintf(log_file, "Usuario desconectado: PID %d\n", usuarios[i].pid);
                    fflush(log_file);
                    limpiar_recursos_usuario(i);
                } else if (result < 0) {
                    if (errno != ECHILD) {
                        perror("Error en waitpid");
                    }
                }
            }
        }

        // 3. Procesar los mensajes de los usuarios activos (no bloqueante)
        for (int i = 0; i < MAX_USUARIOS_SIMULTANEOS; i++) {
            if (usuarios[i].fifo_lectura_fd > 0) { // Asegurarse que el FD es válido
                char buffer[256];
                fd_set set;
                struct timeval timeout;
                
                FD_ZERO(&set);
                FD_SET(usuarios[i].fifo_lectura_fd, &set);
                
                timeout.tv_sec = 0;
                timeout.tv_usec = 1000; // 1ms timeout para no bloquearse
                
                int ready = select(usuarios[i].fifo_lectura_fd + 1, &set, NULL, NULL, &timeout);
                
                if (ready > 0 && FD_ISSET(usuarios[i].fifo_lectura_fd, &set)) {
                    ssize_t nbytes = read(usuarios[i].fifo_lectura_fd, buffer, sizeof(buffer) - 1);
                    
                    if (nbytes > 0) {
                        buffer[nbytes] = '\0';
                        fprintf(log_file, "Usuario (Cuenta %d): %s", usuarios[i].cuenta, buffer);
                        fflush(log_file);
                        printf("Mensaje de usuario %d (Cuenta %d): %s", 
                               i, usuarios[i].cuenta, buffer);
                        
                        // Log adicional para depuración
                        printf("DEBUG: Mensaje recibido (bytes=%zd): %s", nbytes, buffer);
                        
                        // Declarar respuesta y monto aquí para tenerlos disponibles en todo el bloque
                        char respuesta[512];
                        double monto = 0.0;
                        int cuenta_msg = 0; // Para almacenar cuenta extraída del mensaje
                        
                        // Determinar tipo de operación basado en el mensaje
                        if (strstr(buffer, "Depósito") != NULL) {
                            // Respuesta para depósito
                            sscanf(buffer, "[%*[^]]] Depósito de %lf en la cuenta %d", &monto, &cuenta_msg);
                            printf("DEBUG: Extrayendo Depósito - monto=%.2f, cuenta=%d\n", monto, cuenta_msg);
                            

                            // Añadir saldo al archivo de cuentas 
                            sem_wait(sem);
                            printf("Intentando abrir archivo: %s\n", config.archivo_cuentas);
                            printf("DEBUG: Ruta del archivo de cuentas: %s\n", config.archivo_cuentas);
                            FILE *cuentas_file = fopen(config.archivo_cuentas, "r+");
                            if(cuentas_file == NULL){
                                perror("Error al abrir el archivo de cuentas");
                                sem_post(sem);
                                // TODO: decidir como quieres fallar esta comprobación
                            }
                            int cuenta_a_modificar = cuenta_msg; // Número de cuenta extraído del mensaje
                            double monto_a_sumar = monto;       // Monto extraído del mensaje
                            int cuenta_actual;
                            char titular[50];
                            double saldo_actual;
                            int transacciones;
                            long posicion;

                            // Buscar la cuenta en el archivo
                            while ((posicion = ftell(cuentas_file)) >= 0 && 
                                fscanf(cuentas_file, "%d|%49[^|]|%lf|%d\n", &cuenta_actual, titular, &saldo_actual, &transacciones) == 4) {
                                if (cuenta_actual == cuenta_a_modificar) {
                                    // Mover el puntero del archivo a la posición de la línea encontrada
                                    fseek(cuentas_file, posicion, SEEK_SET);

                                    // Sumar el monto al saldo actual
                                    saldo_actual += monto_a_sumar;
                                    transacciones++; // Incrementar el número de transacciones

                                    // Sobrescribir la línea con los nuevos datos
                                    fprintf(cuentas_file, "%d|%s|%.2f|%d\n", cuenta_actual, titular, saldo_actual, transacciones);
                                    printf("Cuenta %d actualizada con nuevo saldo: %.2f\n", cuenta_actual, saldo_actual);
                                    break;
                                }
                            }

                            fclose(cuentas_file); // Cerrar el archivo
                            sem_post(sem);

                            
                            //------- Implementado la suma ------
                            sprintf(respuesta, "Banco: Depósito de %.2f recibido y procesado.\n", monto);
                        } 
                        
                        else if (strstr(buffer, "Retiro") != NULL) {
                            // Respuesta para retiro
                            sscanf(buffer, "[%*[^]]] Retiro de %lf de la cuenta %d", &monto, &cuenta_msg);
                            printf("DEBUG: Extrayendo Retiro - monto=%.2f, cuenta=%d\n", monto, cuenta_msg);
                            sem_wait(sem);
                            printf("Intentando abrir archivo: %s\n", config.archivo_cuentas);
                            printf("DEBUG: Ruta del archivo de cuentas: %s\n", config.archivo_cuentas);
                            FILE *cuentas_file = fopen(config.archivo_cuentas, "r+");

                            if(cuentas_file == NULL){
                                perror("Error al abrir el archivo de cuentas");

                                // Liberar recursos antes de salir
                                sem_close(sem);
                                sem_unlink("/cuentas_semaphore");
                                fclose(log_file);
                            
                                // Salir con un código de error
                                exit(EXIT_FAILURE);
                            }
                            if (monto > config.limite_retiro) {
                                sprintf(respuesta, "Banco: Retiro de %.2f excede el límite permitido de %.2f.\n", 
                                        monto, config.limite_retiro);
                                fclose(cuentas_file);
                                sem_post(sem);
                                continue; 
                            }
                            int cuenta_a_modificar = cuenta_msg; // Número de cuenta extraído del mensaje
                            double monto_a_restar = monto;       // Monto extraído del mensaje
                            int cuenta_actual;
                            char titular[50];
                            double saldo_actual;
                            int transacciones;
                            long posicion;
                            // Buscar la cuenta en el archivo
                            while ((posicion = ftell(cuentas_file)) >= 0 && 
                                fscanf(cuentas_file, "%d|%49[^|]|%lf|%d\n", &cuenta_actual, titular, &saldo_actual, &transacciones) == 4) {
                                if (cuenta_actual == cuenta_a_modificar) {
                                    // Mover el puntero del archivo a la posición de la línea encontrada
                                    fseek(cuentas_file, posicion, SEEK_SET);

                                    // Sumar el monto al saldo actual
                                    saldo_actual -= monto_a_restar;
                                    transacciones++; // Incrementar el número de transacciones

                                    // Sobrescribir la línea con los nuevos datos
                                    fprintf(cuentas_file, "%d|%s|%.2f|%d\n", cuenta_actual, titular, saldo_actual, transacciones);
                                    printf("Cuenta %d actualizada con nuevo saldo: %.2f\n", cuenta_actual, saldo_actual);
                                    break;
                                }
                            }

                            fclose(cuentas_file); // Cerrar el archivo
                            sem_post(sem);
                            sprintf(respuesta, "Banco: Retiro de %.2f procesado.\n", monto);
                        }
                        else if (strstr(buffer, "Transferencia") != NULL) {
                            int cuenta_origen, cuenta_destino;
                            // Asegúrate de que usuario.c envía el mensaje en este formato:
                            // "[Timestamp] Transferencia de MONTO desde la cuenta ORIGEN a la cuenta DESTINO completada."
                            if (sscanf(buffer, "[%*[^]]] Transferencia de %lf desde la cuenta %d a la cuenta %d", &monto, &cuenta_origen, &cuenta_destino) == 3) {
                                printf("DEBUG: Extrayendo Transferencia - monto=%.2f, origen=%d, destino=%d\n", monto, cuenta_origen, cuenta_destino);

                                // --- INICIO LÓGICA DE TRANSFERENCIA ---

                                // 1. Validaciones iniciales rápidas
                                if (cuenta_origen == cuenta_destino) {
                                    sprintf(respuesta, "Banco: Error, la cuenta origen y destino no pueden ser la misma (%d).\n", cuenta_origen);
                                } else if (monto <= 0) {
                                    sprintf(respuesta, "Banco: Error, el monto a transferir debe ser positivo (%.2f).\n", monto);
                                } else if (monto > config.limite_transferencia) {
                                     sprintf(respuesta, "Banco: Error, el monto %.2f excede el límite de transferencia permitido (%d).\n", monto, config.limite_transferencia);
                                } else {
                                    // 2. Adquirir semáforo para acceso exclusivo al archivo
                                    sem_wait(sem);

                                    // 3. Abrir archivo original para leer y temporal para escribir
                                    char temp_filename[] = "data/cuentas.tmp"; // Nombre del archivo temporal
                                    FILE *cuentas_file = fopen(config.archivo_cuentas, "r");
                                    FILE *temp_file = fopen(temp_filename, "w");

                                    if (!cuentas_file || !temp_file) {
                                        perror("Error al abrir archivo de cuentas o temporal para transferencia");
                                        if (cuentas_file) fclose(cuentas_file);
                                        if (temp_file) fclose(temp_file);
                                        sem_post(sem);
                                        sprintf(respuesta, "Banco: Error interno del servidor al procesar transferencia.\n");
                                        // El 'continue' no aplica aquí, se enviará la respuesta al final
                                    } else {
                                        // 4. Variables para guardar datos de las cuentas encontradas
                                        int found_origen = 0, found_destino = 0;
                                        double saldo_origen = -1.0, saldo_destino = -1.0;
                                        int trans_origen = -1, trans_destino = -1;
                                        char titular_origen[50] = "", titular_destino[50] = "";
                                        int error_operacion = 0; // Flag para errores durante la operación

                                        // 5. Leer archivo original, buscar cuentas y validar saldo origen
                                        char line_buffer[256];
                                        int current_cuenta;
                                        char current_titular[50];
                                        double current_saldo;
                                        int current_transacciones;

                                        while (fgets(line_buffer, sizeof(line_buffer), cuentas_file) != NULL) {
                                            // Parsear la línea actual
                                            if (sscanf(line_buffer, "%d|%49[^|]|%lf|%d", &current_cuenta, current_titular, &current_saldo, &current_transacciones) == 4) {
                                                if (current_cuenta == cuenta_origen) {
                                                    found_origen = 1;
                                                    if (current_saldo < monto) {
                                                        sprintf(respuesta, "Banco: Saldo insuficiente en cuenta origen %d (Saldo: %.2f, Solicitado: %.2f).\n", cuenta_origen, current_saldo, monto);
                                                        error_operacion = 1;
                                                        break; // Salir del bucle while, no se puede proceder
                                                    }
                                                    saldo_origen = current_saldo;
                                                    trans_origen = current_transacciones;
                                                    strcpy(titular_origen, current_titular);
                                                } else if (current_cuenta == cuenta_destino) {
                                                    found_destino = 1;
                                                    saldo_destino = current_saldo;
                                                    trans_destino = current_transacciones;
                                                    strcpy(titular_destino, current_titular);
                                                }
                                            }
                                        } // Fin while fgets

                                        // 6. Validar si se encontraron ambas cuentas (si no hubo error de saldo antes)
                                        if (!error_operacion) {
                                            if (!found_origen) {
                                                sprintf(respuesta, "Banco: Error, cuenta origen %d no encontrada.\n", cuenta_origen);
                                                error_operacion = 1;
                                            } else if (!found_destino) {
                                                sprintf(respuesta, "Banco: Error, cuenta destino %d no encontrada.\n", cuenta_destino);
                                                error_operacion = 1;
                                            }
                                        }

                                        // 7. Si todo es válido hasta ahora, proceder a escribir el archivo temporal
                                        if (!error_operacion) {
                                            rewind(cuentas_file); // Volver al inicio del archivo original para leerlo de nuevo

                                            double nuevo_saldo_origen = saldo_origen - monto;
                                            double nuevo_saldo_destino = saldo_destino + monto;
                                            int nuevas_trans_origen = trans_origen + 1;
                                            int nuevas_trans_destino = trans_destino + 1;

                                            while (fgets(line_buffer, sizeof(line_buffer), cuentas_file) != NULL) {
                                                // Parsear de nuevo para saber qué línea es
                                                if (sscanf(line_buffer, "%d|%*[^|]|%*f|%*d", &current_cuenta) == 1) {
                                                    if (current_cuenta == cuenta_origen) {
                                                        // Escribir línea modificada de origen
                                                        fprintf(temp_file, "%d|%s|%.2f|%d\n", cuenta_origen, titular_origen, nuevo_saldo_origen, nuevas_trans_origen);
                                                    } else if (current_cuenta == cuenta_destino) {
                                                        // Escribir línea modificada de destino
                                                        fprintf(temp_file, "%d|%s|%.2f|%d\n", cuenta_destino, titular_destino, nuevo_saldo_destino, nuevas_trans_destino);
                                                    } else {
                                                        // Escribir línea sin modificar
                                                        fputs(line_buffer, temp_file);
                                                    }
                                                } else {
                                                    fputs(line_buffer, temp_file); // Copiar líneas mal formadas o vacías
                                                }
                                            } // Fin while escritura temp_file

                                            // 8. Cerrar archivos
                                            fclose(cuentas_file);
                                            fclose(temp_file);

                                            // 9. Reemplazar archivo original con el temporal
                                            if (remove(config.archivo_cuentas) != 0) {
                                                perror("Error al eliminar el archivo de cuentas original");
                                                remove(temp_filename); // Intentar limpiar el temporal
                                                sprintf(respuesta, "Banco: Error CRÍTICO al actualizar archivo de cuentas (remove).\n");
                                                // Considera loggear este error grave
                                            } else if (rename(temp_filename, config.archivo_cuentas) != 0) {
                                                perror("Error al renombrar el archivo temporal");
                                                sprintf(respuesta, "Banco: Error CRÍTICO al actualizar archivo de cuentas (rename).\n");
                                                // Considera loggear este error grave (original borrado, temporal no renombrado)
                                            } else {
                                                // ¡Éxito! Preparar mensaje de confirmación
                                                sprintf(respuesta, "Banco: Transferencia de %.2f desde %d a %d realizada. Nuevo saldo origen: %.2f\n",
                                                        monto, cuenta_origen, cuenta_destino, nuevo_saldo_origen);
                                                printf("Archivo de cuentas actualizado correctamente tras transferencia.\n");
                                            }
                                        } else {
                                            // Hubo un error (saldo, cuenta no encontrada), cerrar archivos y limpiar temporal
                                            fclose(cuentas_file);
                                            fclose(temp_file);
                                            remove(temp_filename);
                                            printf("Transferencia inválida o fallida. No se modificó el archivo de cuentas.\n");
                                            // 'respuesta' ya contiene el mensaje de error específico
                                        }

                                        // 10. Liberar semáforo
                                        sem_post(sem);
                                    } // Fin else (!cuentas_file || !temp_file)
                                } // Fin else (validaciones iniciales)

                                // --- FIN LÓGICA DE TRANSFERENCIA ---

                            } else {
                                // Error al parsear el mensaje de transferencia del usuario
                                sprintf(respuesta, "Banco: Error al procesar mensaje de transferencia (formato incorrecto).\n");
                                fprintf(stderr, "Error: No se pudo parsear el mensaje de transferencia recibido: %s", buffer);
                            }
                        }
                        else if (strstr(buffer, "Consulta de saldo") != NULL) {
                            sscanf(buffer, "[%*[^]]] Consulta de saldo en la cuenta %d completada.", &cuenta_msg);
                            printf("DEBUG: Consultando saldo para cuenta %d (extraída del mensaje)\n", cuenta_msg);
                            
                            // Consulta de saldo - leer el archivo de cuentas
                            sem_wait(sem);
                            double saldo = -1;
                            
                            // Log para depuración: verificar la ruta del archivo
                            printf("Intentando abrir archivo: %s\n", config.archivo_cuentas);
                            printf("DEBUG: Ruta del archivo de cuentas: %s\n", config.archivo_cuentas);
                            FILE *cuentas_file = fopen(config.archivo_cuentas, "r");
                            

                            if (cuentas_file) {
                                int current_cuenta;
                                char titular[50];
                                double current_saldo;
                                int transacciones;
                                printf("DEBUG: Buscando cuenta %d en archivo de cuentas\n", usuarios[i].cuenta);
                                
                                // Leer el archivo usando '|' como delimitador
                                while (fscanf(cuentas_file, "%d|%49[^|]|%lf|%d", 
                                              &current_cuenta, titular, &current_saldo, &transacciones) == 4) {
                                    printf("Leyendo: Cuenta=%d, Titular=%s, Saldo=%.2f, Transacciones=%d\n", 
                                           current_cuenta, titular, current_saldo, transacciones);
                                    if (current_cuenta == usuarios[i].cuenta) {
                                        saldo = current_saldo;
                                        break;
                                    }
                                }
                                fclose(cuentas_file);
                                
                                if (saldo >= 0) {
                                    sprintf(respuesta, "Banco: Saldo actual de la cuenta %d: $%.2f\n", 
                                            usuarios[i].cuenta, saldo);
                                    printf("DEBUG: Saldo encontrado: %.2f\n", saldo);
                                } else {
                                    sprintf(respuesta, "Banco: No se encontró la cuenta %d en el registro.\n", 
                                            usuarios[i].cuenta);
                                    printf("DEBUG: No se encontró la cuenta en el archivo\n");
                                }
                            } else {
                                sprintf(respuesta, "Banco: Error al acceder al archivo de cuentas.\n");
                                perror("Error al abrir archivo de cuentas");
                            }
                            sem_post(sem);
                        } else {
                            // Mensaje genérico para otros casos
                            sprintf(respuesta, "Banco: Mensaje recibido. Procesando...\n");
                        }
                        
                        // Enviar respuesta apropiada al usuario
                        if (usuarios[i].fifo_escritura_fd > 0) {
                            if (write(usuarios[i].fifo_escritura_fd, respuesta, strlen(respuesta)) < 0) {
                                if (errno != EAGAIN && errno != EWOULDBLOCK) {
                                    perror("Error al escribir respuesta al usuario");
                                }
                            }
                        }
                    }
                    else if (nbytes == 0) { // EOF - el FIFO se cerró - CORRECCIÓN: Mover dentro del bloque correcto
                        printf("Detectado EOF en FIFO de lectura del usuario %d (PID %d). Cerrando conexión.\n", 
                               usuarios[i].cuenta, usuarios[i].pid);
                        close(usuarios[i].fifo_lectura_fd);
                        usuarios[i].fifo_lectura_fd = 0;
                    }
                    else { // Error en read
                        if (errno != EAGAIN && errno != EWOULDBLOCK) {
                            perror("Error al leer del FIFO del usuario");
                            limpiar_recursos_usuario(i);
                        }
                        // Si es EAGAIN/EWOULDBLOCK es normal con non-blocking, no hacer nada
                    }
                } // Fin del if (ready > 0...)
            }
        }
        
        // Pequeña pausa para no saturar la CPU
        usleep(100000); // 100ms
    }

    // Esperar a que todos los procesos hijos terminen
    printf("Finalizando todos los procesos de usuario...\n");
    for (int i = 0; i < MAX_USUARIOS_SIMULTANEOS; i++) {
        if (usuarios[i].pid > 0) {
            kill(usuarios[i].pid, SIGTERM);
            waitpid(usuarios[i].pid, NULL, 0);
            limpiar_recursos_usuario(i);
        }
    }

    // Cierre de recursos.
    fclose(log_file);
    sem_close(sem);
    sem_unlink("/cuentas_semaphore");

    printf("Proceso del banco finalizado correctamente.\n");
    return EXIT_SUCCESS;
}
