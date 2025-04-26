#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <string.h>
#include <pthread.h>
#include <fcntl.h>
#include <time.h>
#include <signal.h>
#include <errno.h>

#define BUFFER_SIZE 256
#define LOG_FILE "../data/transacciones.log"

// Definición de la estructura Operacion.
typedef struct {
    int tipo_operacion; // 1: Depósito, 2: Retiro, 3: Transferencia, 4: Consultar saldo
    double monto;
    int cuenta;
    int cuenta_destino; // Para operaciones de transferencia
    // Descriptor de archivo para escribir al banco
    int fifo_fd;
} Operacion;

// Estructura para pasar parámetros al hilo.
typedef struct {
    Operacion op;
} OperacionArgs;

// Variables globales para los FIFOs
char fifo_escritura[256]; // Usuario escribe aquí, banco lee
char fifo_lectura[256];   // Banco escribe aquí, usuario lee
int fifo_escritura_fd = -1;
int fifo_lectura_fd = -1;

// Mutex para sincronizar la salida
pthread_mutex_t stdout_mutex = PTHREAD_MUTEX_INITIALIZER;

// Manejador para cerrar apropiadamente
void manejador_terminar(int sig) {
    printf("\nTerminando sesión...\n");
    
    if (fifo_escritura_fd >= 0) {
        close(fifo_escritura_fd);
    }
    
    if (fifo_lectura_fd >= 0) {
        close(fifo_lectura_fd);
    }
    
    exit(0);
}

// Función para obtener timestamp actual
void get_timestamp(char *buffer, size_t size) {
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    strftime(buffer, size, "%Y-%m-%d %H:%M:%S", tm_info);
}

// Función que ejecuta la operación y comunica con el banco
void *ejecutar_operacion(void *arg) {
    OperacionArgs *args = (OperacionArgs *)arg;
    char mensaje[BUFFER_SIZE];
    char timestamp[30];
    get_timestamp(timestamp, sizeof(timestamp));
    
    // Bloquear el mutex para asegurar que el mensaje completo se escriba de una vez
    pthread_mutex_lock(&stdout_mutex);
    
    // Crear mensaje basado en el tipo de operación
    switch (args->op.tipo_operacion) {
        case 1:
            sprintf(mensaje, "[%s] Depósito de %.2f en la cuenta %d completado.\n", 
                   timestamp, args->op.monto, args->op.cuenta);
            break;
        case 2:
            sprintf(mensaje, "[%s] Retiro de %.2f de la cuenta %d completado.\n", 
                   timestamp, args->op.monto, args->op.cuenta);
            break;
        case 3:
        // Asumiendo que args->op tiene un campo cuenta_destino
        sprintf(mensaje, "[%s] Transferencia de %.2f desde la cuenta %d a la cuenta %d completada.\n",
            timestamp, args->op.monto, args->op.cuenta, args->op.cuenta_destino); //op.cuenta_destino
        break;

        case 4:
            sprintf(mensaje, "[%s] Consulta de saldo en la cuenta %d completada.\n", 
                   timestamp, args->op.cuenta);
            break;
        default:
            sprintf(mensaje, "[%s] Operación desconocida en la cuenta %d.\n", 
                   timestamp, args->op.cuenta);
            break;
    }
    
    // Mostrar en pantalla para el usuario
    printf("%s", mensaje);
    
    // Enviar mensaje al banco a través del FIFO
    if (fifo_escritura_fd >= 0) {
        // Si el FIFO se cerró, intentamos reabrirlo
        if (fifo_escritura_fd == -1) {
            fifo_escritura_fd = open(fifo_escritura, O_WRONLY);
        }
        
        if (fifo_escritura_fd >= 0) {
            if (write(fifo_escritura_fd, mensaje, strlen(mensaje)) < 0) {
                perror("Error al escribir en FIFO");
            }
        } else {
            perror("No se pudo abrir el FIFO para escritura");
        }
    }
    
    // Desbloquear el mutex
    pthread_mutex_unlock(&stdout_mutex);

    free(args);
    pthread_exit(NULL);
}

// Función para leer mensajes del banco
void *leer_mensajes(void *arg) {
    char buffer[BUFFER_SIZE];
    ssize_t bytes_leidos;
    
    while (1) {
        if (fifo_lectura_fd >= 0) {
            bytes_leidos = read(fifo_lectura_fd, buffer, sizeof(buffer) - 1);
            
            if (bytes_leidos > 0) {
                buffer[bytes_leidos] = '\0';
                
                // Bloquear el mutex para imprimir el mensaje
                pthread_mutex_lock(&stdout_mutex);
                printf("\n[Mensaje del Banco]: %s\n", buffer);
                pthread_mutex_unlock(&stdout_mutex);
            } else if (bytes_leidos == -1 && errno != EAGAIN && errno != EWOULDBLOCK) {
                perror("Error al leer del FIFO");
                break;
            }
        }
        
        // Esperar un poco antes de intentar leer de nuevo
        usleep(100000); // 100ms
    }
    
    return NULL;
}

// Función que muestra el menú interactivo y lanza un hilo por cada operación.
void menu_usuario(int cuenta) {
    int opcion;
    double monto;
    int cuenta_destino_input;//variable para la cuenta destino
    pthread_t tid;

    printf("\n¡Bienvenido al Sistema Bancario!\n");
    printf("Sesión iniciada para la cuenta: %d\n", cuenta);
    printf("Comunicación establecida con el servidor del banco.\n");

    while (1) {
        printf("\nMenú de Usuario (Cuenta %d):\n", cuenta);
        printf("1. Depósito\n2. Retiro\n3. Transferencia\n4. Consultar saldo\n5. Salir\n");
        printf("Seleccione una opción: ");
        if (scanf("%d", &opcion) != 1) {
            fprintf(stderr, "Entrada inválida. Intente de nuevo.\n");
            while (getchar() != '\n'); // Limpiar el buffer de entrada.
            continue;
        }

        // Si se selecciona 'Salir', se termina el proceso.
        if (opcion == 5) {
            printf("Sesión finalizada para la cuenta %d.\n", cuenta);
            break;
        }
        
        Operacion op;
        op.tipo_operacion = opcion;
        op.monto = 0.0;
        op.cuenta = cuenta;
        op.cuenta_destino = 0; // Para operaciones de transferencia inicializar a 0
        op.fifo_fd = fifo_escritura_fd;

        // Para operaciones que requieren monto.
        if (opcion >= 1 && opcion <= 3) {
            printf("Ingrese el monto: ");
            if (scanf("%lf", &monto) != 1) {
                fprintf(stderr, "Monto inválido. Intente de nuevo.\n");
                while (getchar() != '\n');
                continue;
            }
            op.monto = monto;
        }
        if (opcion == 3) {
            printf("Ingrese la cuenta destino: ");
            if (scanf("%d", &cuenta_destino_input) != 1) {
                fprintf(stderr, "Cuenta destino inválida. Intente de nuevo.\n");
                while (getchar() != '\n');
                continue;
            }
            // Validar que no sea la misma cuenta
            if (cuenta_destino_input == cuenta) {
                 fprintf(stderr, "Error: La cuenta destino no puede ser la misma que la cuenta origen (%d).\n", cuenta);
                 continue;
            }
            op.cuenta_destino = cuenta_destino_input; // Guardar la cuenta destino
        }
        
        // Preparar los argumentos para el hilo.
        OperacionArgs *args = malloc(sizeof(OperacionArgs));
        if (args == NULL) {
            perror("Error al asignar memoria para OperacionArgs");
            continue;
        }
        args->op = op;
        
        // Crear un hilo para ejecutar la operación.
        if (pthread_create(&tid, NULL, ejecutar_operacion, (void *)args) != 0) {
            perror("Error al crear el hilo");
            free(args);
            continue;
        }
        
        // Desvincular el hilo para que se limpie automáticamente al finalizar.
        pthread_detach(tid);
    }

    // Enviar mensaje de cierre al banco
    char mensaje[BUFFER_SIZE];
    sprintf(mensaje, "Usuario con cuenta %d ha cerrado sesión.\n", cuenta);
    
    if (fifo_escritura_fd >= 0) {
        if (write(fifo_escritura_fd, mensaje, strlen(mensaje)) < 0) {
            perror("Error al enviar mensaje de cierre");
        }
    }
    
    // Cerrar FIFOs
    if (fifo_escritura_fd >= 0) close(fifo_escritura_fd);
    if (fifo_lectura_fd >= 0) close(fifo_lectura_fd);
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Uso: %s <numero_cuenta> [fifo_escritura] [fifo_lectura]\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    int numero_cuenta = atoi(argv[1]);
    
    // Configurar manejadores de señales
    signal(SIGTERM, manejador_terminar);
    signal(SIGINT, manejador_terminar);
    
    // Imprimir el PID del proceso usuario
    printf("Proceso usuario iniciado con PID: %d\n", getpid());
    
    // Si se proporcionaron nombres de FIFOs, usarlos para la comunicación
    if (argc >= 4) {
        strcpy(fifo_escritura, argv[2]); // Usuario -> Banco
        strcpy(fifo_lectura, argv[3]);   // Banco -> Usuario
        
        printf("Iniciando conexión con el banco...\n");
        printf("FIFO escritura: %s\n", fifo_escritura);
        printf("FIFO lectura: %s\n", fifo_lectura);
        
        // Abrir FIFO para escritura - usando modo bloqueante para operaciones críticas
        printf("Abriendo FIFO para escritura...\n");
        fifo_escritura_fd = open(fifo_escritura, O_RDWR);
        if (fifo_escritura_fd < 0) {
            perror("Error crítico al abrir FIFO para escritura");
            exit(EXIT_FAILURE);
        }
        printf("FIFO de escritura abierto correctamente (fd=%d)\n", fifo_escritura_fd);
        
        // Abrir FIFO para lectura - primero en modo bloqueante
        printf("Abriendo FIFO para lectura...\n");
        fifo_lectura_fd = open(fifo_lectura, O_RDWR);
        if (fifo_lectura_fd < 0) {
            perror("Error crítico al abrir FIFO para lectura");
            close(fifo_escritura_fd);
            exit(EXIT_FAILURE);
        }
        printf("FIFO de lectura abierto correctamente (fd=%d)\n", fifo_lectura_fd);
        
        // Configurar modo no bloqueante para el FIFO de lectura después de establecer la conexión
        fcntl(fifo_lectura_fd, F_SETFL, fcntl(fifo_lectura_fd, F_GETFL) | O_NONBLOCK);
        
        // Crear un hilo para leer mensajes del banco
        pthread_t hilo_lector;
        if (pthread_create(&hilo_lector, NULL, leer_mensajes, NULL) != 0) {
            perror("Error al crear hilo para leer mensajes");
        } else {
            pthread_detach(hilo_lector);
        }
        
        // Enviar mensaje de inicio
        char mensaje[BUFFER_SIZE];
        sprintf(mensaje, "Usuario con cuenta %d ha iniciado sesión.\n", numero_cuenta);
        if (write(fifo_escritura_fd, mensaje, strlen(mensaje)) < 0) {
            perror("Error al enviar mensaje de inicio");
        }
    }
    
    menu_usuario(numero_cuenta);

    return EXIT_SUCCESS;
}
