#define _DEFAULT_SOURCE  //para habilitar usleep()
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <string.h>
#include <pthread.h>
#include <fcntl.h>
#include <time.h>
#include <sys/shm.h> // Para memoria compartida
#include <signal.h>
#include <errno.h>

#define BUFFER_SIZE 256
#define LOG_FILE "../data/transacciones.log"


// Definición de la estructura Operacion.
// Esta estructura se usa para pasar datos al hilo que envía el mensaje al banco.
// Con SHM, el hilo podría también realizar la operación en SHM directamente.
typedef struct {
    int tipo_operacion; // 1: Depósito, 2: Retiro, 3: Transferencia, 4: Consultar saldo
    double monto;
    int cuenta;
    int cuenta_destino; // Para operaciones de transferencia
} Operacion;

typedef struct {
    int numero_cuenta;
    char titular[50];
    float saldo;
    int bloqueado;
} Cuenta;

typedef struct {
    pthread_mutex_t mutex; // Para acceder al mutex desde el proceso hijo
    Cuenta cuentas[100]; // Asumir MAX_CUENTAS 100, debe coincidir con banco.c
    int num_cuentas;
} TablaCuentas;

// Estructura para pasar parámetros al hilo.
typedef struct {
    Operacion op;
} OperacionArgs;

// Variables globales para los FIFOs
char fifo_escritura[256]; // Usuario escribe aquí, banco lee
char fifo_lectura[256];   // Banco escribe aquí, usuario lee
int fifo_escritura_fd = -1;
int fifo_lectura_fd = -1;
TablaCuentas *tabla_bancaria_shm = NULL; // Puntero a la memoria compartida
int shm_id_usuario = -1; // ID del segmento de memoria compartida recibido del banco

// Mutex para sincronizar la salida
pthread_mutex_t stdout_mutex = PTHREAD_MUTEX_INITIALIZER;

//prototipo de la función timestamp
void get_timestamp(char *buffer, size_t size);

// Función que ejecuta la operación y comunica con el banco
void *ejecutar_operacion(void *arg) {
    OperacionArgs *args = (OperacionArgs *)arg;
    char mensaje_al_banco[BUFFER_SIZE];
    char mensaje_usuario_local[BUFFER_SIZE]; // Mensaje para mostrar localmente
    char timestamp[30];
    get_timestamp(timestamp, sizeof(timestamp));
    int cuenta_idx = -1;
    int cuenta_destino_idx = -1;
    float saldo_actual = 0.0;
    float saldo_actual_destino = 0.0;

    if (tabla_bancaria_shm == NULL) {
        sprintf(mensaje_usuario_local, "Error: Memoria compartida no accesible.\n");
        pthread_mutex_lock(&stdout_mutex);
        printf("%s", mensaje_usuario_local);
        pthread_mutex_unlock(&stdout_mutex);
        free(args);
        pthread_exit(NULL);
    }

    // Bloquear mutex de la memoria compartida
    if (pthread_mutex_lock(&tabla_bancaria_shm->mutex) != 0) {
        perror("Usuario: Error al bloquear mutex de SHM");
        sprintf(mensaje_usuario_local, "Error interno al procesar operación (mutex lock).\n");
        pthread_mutex_lock(&stdout_mutex);
        printf("%s", mensaje_usuario_local);
        pthread_mutex_unlock(&stdout_mutex);
        free(args);
        pthread_exit(NULL);
    }

    // Encontrar la cuenta de origen
    for (int i = 0; i < tabla_bancaria_shm->num_cuentas; i++) {
        if (tabla_bancaria_shm->cuentas[i].numero_cuenta == args->op.cuenta) {
            cuenta_idx = i;
            break;
        }
    }

    if (cuenta_idx == -1) {
        sprintf(mensaje_usuario_local, "Error: Cuenta origen %d no encontrada.\n", args->op.cuenta);
    } else if (tabla_bancaria_shm->cuentas[cuenta_idx].bloqueado) {
        sprintf(mensaje_usuario_local, "Error: Cuenta origen %d está bloqueada.\n", args->op.cuenta);
    } else {
        // Procesar operación
        switch (args->op.tipo_operacion) {
            case 1: // Depósito
                tabla_bancaria_shm->cuentas[cuenta_idx].saldo += args->op.monto;
                saldo_actual = tabla_bancaria_shm->cuentas[cuenta_idx].saldo;
                sprintf(mensaje_usuario_local, "Depósito de %.2f realizado. Nuevo saldo: %.2f\n", args->op.monto, saldo_actual);
                sprintf(mensaje_al_banco, "[%s] Depósito de %.2f en la cuenta %d. Nuevo Saldo: %.2f\n",
                       timestamp, args->op.monto, args->op.cuenta, saldo_actual);
                break;
            case 2: // Retiro
                if (tabla_bancaria_shm->cuentas[cuenta_idx].saldo >= args->op.monto) {
                    tabla_bancaria_shm->cuentas[cuenta_idx].saldo -= args->op.monto;
                    saldo_actual = tabla_bancaria_shm->cuentas[cuenta_idx].saldo;
                    sprintf(mensaje_usuario_local, "Retiro de %.2f realizado. Nuevo saldo: %.2f\n", args->op.monto, saldo_actual);
                    sprintf(mensaje_al_banco, "[%s] Retiro de %.2f de la cuenta %d. Nuevo Saldo: %.2f\n",
                           timestamp, args->op.monto, args->op.cuenta, saldo_actual);
                } else {
                    sprintf(mensaje_usuario_local, "Error: Saldo insuficiente para retiro (%.2f) en cuenta %d. Saldo actual: %.2f\n",
                           args->op.monto, args->op.cuenta, tabla_bancaria_shm->cuentas[cuenta_idx].saldo);
                    sprintf(mensaje_al_banco, "[%s] Intento de Retiro FALLIDO (saldo insuficiente) de %.2f de la cuenta %d.\n",
                           timestamp, args->op.monto, args->op.cuenta);
                }
                break;
            case 3: // Transferencia
                // Encontrar cuenta destino
                for (int i = 0; i < tabla_bancaria_shm->num_cuentas; i++) {
                    if (tabla_bancaria_shm->cuentas[i].numero_cuenta == args->op.cuenta_destino) {
                        cuenta_destino_idx = i;
                        break;
                    }
                }
                if (cuenta_destino_idx == -1) {
                    sprintf(mensaje_usuario_local, "Error: Cuenta destino %d no encontrada.\n", args->op.cuenta_destino);
                    sprintf(mensaje_al_banco, "[%s] Intento de Transferencia FALLIDO (cuenta destino %d no encontrada) desde %d.\n",
                           timestamp, args->op.cuenta_destino, args->op.cuenta);
                } else if (tabla_bancaria_shm->cuentas[cuenta_destino_idx].bloqueado) {
                     sprintf(mensaje_usuario_local, "Error: Cuenta destino %d está bloqueada.\n", args->op.cuenta_destino);
                     sprintf(mensaje_al_banco, "[%s] Intento de Transferencia FALLIDO (cuenta destino %d bloqueada) desde %d.\n",
                           timestamp, args->op.cuenta_destino, args->op.cuenta);
                } else if (tabla_bancaria_shm->cuentas[cuenta_idx].saldo >= args->op.monto) {
                    tabla_bancaria_shm->cuentas[cuenta_idx].saldo -= args->op.monto;
                    tabla_bancaria_shm->cuentas[cuenta_destino_idx].saldo += args->op.monto;
                    saldo_actual = tabla_bancaria_shm->cuentas[cuenta_idx].saldo;
                    saldo_actual_destino = tabla_bancaria_shm->cuentas[cuenta_destino_idx].saldo;
                    sprintf(mensaje_usuario_local, "Transferencia de %.2f a cuenta %d realizada. Nuevo saldo origen: %.2f. Nuevo saldo destino: %.2f\n",
                           args->op.monto, args->op.cuenta_destino, saldo_actual, saldo_actual_destino);
                    sprintf(mensaje_al_banco, "[%s] Transferencia de %.2f desde la cuenta %d a la cuenta %d. Saldo origen: %.2f. Saldo destino: %.2f\n",
                           timestamp, args->op.monto, args->op.cuenta, args->op.cuenta_destino, saldo_actual, saldo_actual_destino);
                } else {
                    sprintf(mensaje_usuario_local, "Error: Saldo insuficiente para transferir (%.2f) desde cuenta %d. Saldo actual: %.2f\n",
                           args->op.monto, args->op.cuenta, tabla_bancaria_shm->cuentas[cuenta_idx].saldo);
                    sprintf(mensaje_al_banco, "[%s] Intento de Transferencia FALLIDO (saldo insuficiente) de %.2f desde cuenta %d a %d.\n",
                           timestamp, args->op.monto, args->op.cuenta, args->op.cuenta_destino);
                }
                break;
            case 4: // Consultar saldo
                saldo_actual = tabla_bancaria_shm->cuentas[cuenta_idx].saldo;
                sprintf(mensaje_usuario_local, "Saldo actual de la cuenta %d: %.2f\n", args->op.cuenta, saldo_actual);
                sprintf(mensaje_al_banco, "[%s] Consulta de saldo en la cuenta %d. Saldo actual: %.2f\n",
                       timestamp, args->op.cuenta, saldo_actual);
                break;
            default:
                sprintf(mensaje_usuario_local, "Operación desconocida.\n");
                sprintf(mensaje_al_banco, "[%s] Operación desconocida en la cuenta %d.\n",
                       timestamp, args->op.cuenta);
                break;
        }
    }

    // Desbloquear mutex de la memoria compartida
    if (pthread_mutex_unlock(&tabla_bancaria_shm->mutex) != 0) {
        perror("Usuario: Error al desbloquear mutex de SHM");
        // Continuar de todas formas para no dejar el mutex bloqueado indefinidamente si es posible
    }

    // Mostrar mensaje local al usuario (sincronizado con stdout_mutex)
    pthread_mutex_lock(&stdout_mutex);
    printf("\n%s", mensaje_usuario_local);
    // Re-imprimir prompt si es necesario o dejar que el bucle principal lo haga
    printf("Seleccione una opción: "); fflush(stdout);
    pthread_mutex_unlock(&stdout_mutex);

    // Enviar mensaje al banco a través del FIFO
    if (fifo_escritura_fd >= 0) {
        if (write(fifo_escritura_fd, mensaje_al_banco, strlen(mensaje_al_banco)) < 0) {
            perror("Error al escribir en FIFO al banco");
        }
    } else {
        fprintf(stderr, "Error: FIFO de escritura al banco no disponible.\n");
    }

    free(args);
    pthread_exit(NULL);
}

// Función que muestra el menú interactivo.
// Las operaciones ahora se ejecutan en un hilo que accede a SHM.
void menu_usuario(int cuenta) {
    int opcion;
    double monto_input;
    int cuenta_destino_input;
    pthread_t tid_operacion; // ID del hilo para la operación

    // Mensaje de bienvenida ya se maneja en main o por el banco

    while (1) {
        // El prompt se imprime desde el hilo lector o aquí si no hay mensajes pendientes
        pthread_mutex_lock(&stdout_mutex);
        printf("\nMenú de Usuario (Cuenta %d):\n", cuenta);
        printf("1. Depósito\n2. Retiro\n3. Transferencia\n4. Consultar saldo\n5. Salir\n");
        printf("Seleccione una opción: ");
        fflush(stdout); // Asegurar que el prompt se muestre antes de scanf
        pthread_mutex_unlock(&stdout_mutex);

        if (scanf("%d", &opcion) != 1) {
            pthread_mutex_lock(&stdout_mutex);
            fprintf(stderr, "Entrada inválida. Intente de nuevo.\n");
            pthread_mutex_unlock(&stdout_mutex);
            while (getchar() != '\n'); // Limpiar buffer
            continue;
        }

        if (opcion == 5) {
            break; // Salir del bucle para finalizar
        }

        if (opcion < 1 || opcion > 4) {
            pthread_mutex_lock(&stdout_mutex);
            fprintf(stderr, "Opción no válida. Intente de nuevo.\n");
            pthread_mutex_unlock(&stdout_mutex);
            continue;
        }
        
        Operacion op_actual;
        op_actual.tipo_operacion = opcion;
        op_actual.cuenta = cuenta;
        op_actual.monto = 0.0;
        op_actual.cuenta_destino = 0;

        if (opcion == 1 || opcion == 2 || opcion == 3) { // Depósito, Retiro, Transferencia
            pthread_mutex_lock(&stdout_mutex);
            printf("Ingrese el monto: "); fflush(stdout);
            pthread_mutex_unlock(&stdout_mutex);
            if (scanf("%lf", &monto_input) != 1 || monto_input <= 0) {
                pthread_mutex_lock(&stdout_mutex);
                fprintf(stderr, "Monto inválido (debe ser > 0). Intente de nuevo.\n");
                pthread_mutex_unlock(&stdout_mutex);
                while (getchar() != '\n');
                continue;
            }
            op_actual.monto = monto_input;
        }

        if (opcion == 3) { // Transferencia
            pthread_mutex_lock(&stdout_mutex);
            printf("Ingrese la cuenta destino: "); fflush(stdout);
            pthread_mutex_unlock(&stdout_mutex);
            if (scanf("%d", &cuenta_destino_input) != 1) {
                pthread_mutex_lock(&stdout_mutex);
                fprintf(stderr, "Cuenta destino inválida. Intente de nuevo.\n");
                pthread_mutex_unlock(&stdout_mutex);
                while (getchar() != '\n');
                continue;
            }
            if (cuenta_destino_input == cuenta) {
                pthread_mutex_lock(&stdout_mutex);
                fprintf(stderr, "Error: La cuenta destino no puede ser la misma que la cuenta origen (%d).\n", cuenta);
                pthread_mutex_unlock(&stdout_mutex);
                continue;
            }
            op_actual.cuenta_destino = cuenta_destino_input;
        }
        
        OperacionArgs *args_hilo = malloc(sizeof(OperacionArgs));
        if (args_hilo == NULL) {
            perror("Error al asignar memoria para OperacionArgs");
            continue;
        }
        args_hilo->op = op_actual;
        
        if (pthread_create(&tid_operacion, NULL, ejecutar_operacion, (void *)args_hilo) != 0) {
            perror("Error al crear el hilo de operación");
            free(args_hilo);
        } else {
            pthread_detach(tid_operacion); // El hilo se limpiará solo
        }
        // Pequeña pausa para permitir que el hilo de operación comience y posiblemente imprima algo
        usleep(50000); 
    }
}

// Manejador para cerrar apropiadamente
void manejador_terminar(int sig) {
    printf("\nTerminando sesión...\n");
    
    if (fifo_escritura_fd >= 0) {
        close(fifo_escritura_fd);
    }
    
    if (fifo_lectura_fd >= 0) {
        close(fifo_lectura_fd);
    }
    
    if (tabla_bancaria_shm != NULL) {
        shmdt(tabla_bancaria_shm);
        tabla_bancaria_shm = NULL;
    }

    exit(0);
}

// Función para obtener timestamp actual
char timestamp[30];
void get_timestamp(char *buffer, size_t size) {
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    strftime(buffer, size, "%Y-%m-%d %H:%M:%S", tm_info);
}



// Función para leer mensajes del banco (respuestas/confirmaciones)
void *leer_mensajes_banco(void *arg) {
    char buffer[BUFFER_SIZE];
    ssize_t bytes_leidos;
    
    while (1) {
        if (fifo_lectura_fd >= 0) {
            bytes_leidos = read(fifo_lectura_fd, buffer, sizeof(buffer) - 1);
            
            if (bytes_leidos > 0) {
                buffer[bytes_leidos] = '\0';
                
                pthread_mutex_lock(&stdout_mutex);
                printf("\n[Respuesta del Banco]: %s", buffer);
                // Re-imprimir prompt si es necesario o dejar que el bucle principal lo haga
                printf("Seleccione una opción: "); fflush(stdout);
                pthread_mutex_unlock(&stdout_mutex);
            } else if (bytes_leidos == 0) { // EOF, el banco cerró el pipe
                pthread_mutex_lock(&stdout_mutex);
                printf("\nEl banco ha cerrado la conexión.\n");
                pthread_mutex_unlock(&stdout_mutex);
                // Podría ser necesario terminar el proceso usuario aquí
                // close(fifo_lectura_fd); fifo_lectura_fd = -1; // Marcar como cerrado
                // exit(0); // O una forma más controlada
                break; 
            } else if (bytes_leidos == -1 && errno != EAGAIN && errno != EWOULDBLOCK) {
                perror("Error al leer del FIFO del banco");
                break; // Salir del bucle en caso de error persistente
            }
        } else {
            // Si el FIFO no está abierto, esperar y reintentar o salir.
            // Por ahora, solo una pausa.
        }
        
        usleep(100000); // Pausa para no consumir CPU excesivamente
    }
    
    return NULL;
}



int main(int argc, char *argv[]) {
    if (argc < 5) { // numero_programa, cuenta, fifo_esc, fifo_lec, shm_id
        fprintf(stderr, "Uso: %s <numero_cuenta> <fifo_escritura_a_banco> <fifo_lectura_de_banco> <shm_id>\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    int numero_cuenta = atoi(argv[1]);
    strcpy(fifo_escritura, argv[2]); // Usuario escribe aquí (a banco), banco lee
    strcpy(fifo_lectura, argv[3]);   // Banco escribe aquí (a usuario), usuario lee
    shm_id_usuario = atoi(argv[4]);
    
    signal(SIGTERM, manejador_terminar);
    signal(SIGINT, manejador_terminar);
    
    printf("Proceso usuario iniciado con PID: %d para cuenta %d\n", getpid(), numero_cuenta);
    printf("Intentando conectar a SHM ID: %d\n", shm_id_usuario);

    // Adjuntar a la memoria compartida
    tabla_bancaria_shm = (TablaCuentas *)shmat(shm_id_usuario, NULL, 0);
    if (tabla_bancaria_shm == (void *)-1) {
        perror("Usuario: Error en shmat");
        exit(EXIT_FAILURE);
    }
    printf("Conectado a memoria compartida. Número de cuentas en SHM (según banco): %d\n", tabla_bancaria_shm->num_cuentas);

    // Abrir FIFOs
    printf("Abriendo FIFO para escritura a banco: %s\n", fifo_escritura);
    fifo_escritura_fd = open(fifo_escritura, O_WRONLY); // O_RDWR si el banco también escribe en este para alguna razon, pero usualmente O_WRONLY
    if (fifo_escritura_fd < 0) {
        perror("Usuario: Error crítico al abrir FIFO para escritura a banco");
        shmdt(tabla_bancaria_shm);
        exit(EXIT_FAILURE);
    }
    printf("FIFO de escritura a banco abierto (fd=%d)\n", fifo_escritura_fd);

    printf("Abriendo FIFO para lectura de banco: %s\n", fifo_lectura);
    fifo_lectura_fd = open(fifo_lectura, O_RDONLY); // O_RDWR si el usuario también escribe en este, pero usualmente O_RDONLY
    if (fifo_lectura_fd < 0) {
        perror("Usuario: Error crítico al abrir FIFO para lectura de banco");
        close(fifo_escritura_fd);
        shmdt(tabla_bancaria_shm);
        exit(EXIT_FAILURE);
    }
    printf("FIFO de lectura de banco abierto (fd=%d)\n", fifo_lectura_fd);

    // Configurar modo no bloqueante para el FIFO de lectura del banco
    // Esto es importante para que el hilo `leer_mensajes_banco` no se bloquee indefinidamente
    // y pueda ser interrumpido o verificar periódicamente.
    int flags = fcntl(fifo_lectura_fd, F_GETFL, 0);
    if (fcntl(fifo_lectura_fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        perror("Usuario: fcntl O_NONBLOCK en FIFO lectura");
        // Continuar, pero la lectura podría ser bloqueante o fallar de otras maneras
    }

    // Crear hilo para leer respuestas del banco
    pthread_t hilo_lector_banco;
    if (pthread_create(&hilo_lector_banco, NULL, leer_mensajes_banco, NULL) != 0) {
        perror("Error al crear hilo para leer mensajes del banco");
        // Considerar manejo de error, como cerrar y salir
    } else {
        pthread_detach(hilo_lector_banco); // El hilo se limpiará solo al terminar
    }

    // Enviar mensaje de inicio de sesión al banco
    char msg_inicio[BUFFER_SIZE];
    get_timestamp(timestamp, sizeof(timestamp));
    sprintf(msg_inicio, "[%s] Usuario con cuenta %d ha iniciado sesión y conectado a SHM.\n", timestamp, numero_cuenta);
    if (write(fifo_escritura_fd, msg_inicio, strlen(msg_inicio)) < 0) {
        perror("Error al enviar mensaje de inicio de sesión al banco");
    }

    menu_usuario(numero_cuenta); // Iniciar menú interactivo

    // Al salir del menú (opción 5)
    printf("Cerrando sesión de usuario %d...\n", numero_cuenta);
    
    char msg_cierre[BUFFER_SIZE];
    get_timestamp(timestamp, sizeof(timestamp));
    sprintf(msg_cierre, "[%s] Usuario con cuenta %d ha cerrado sesión.\n", timestamp, numero_cuenta);
    if (fifo_escritura_fd >= 0) {
        if (write(fifo_escritura_fd, msg_cierre, strlen(msg_cierre)) < 0) {
            perror("Error al enviar mensaje de cierre de sesión al banco");
        }
        close(fifo_escritura_fd);
        fifo_escritura_fd = -1;
    }

    if (fifo_lectura_fd >= 0) {
        close(fifo_lectura_fd);
        fifo_lectura_fd = -1;
    }

    if (tabla_bancaria_shm != NULL) {
        shmdt(tabla_bancaria_shm);
        tabla_bancaria_shm = NULL;
    }

    printf("Usuario %d finalizado.\n", numero_cuenta);
    return EXIT_SUCCESS;
}


