#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h> // Para mkdir
#include <errno.h>    // Para errno

// Definición de la estructura Cuenta (debe ser idéntica a la de banco.c y usuario.c)
typedef struct {
    int numero_cuenta;
    char titular[50];
    float saldo;
    int bloqueado; // 0 para activa, 1 para bloqueada
} Cuenta;

int main(void) {
    // Ruta del archivo de cuentas
    const char *ruta_archivo = "../data/cuentas.dat";
    
    // Crear directorio ../data/ si no existe
    if (mkdir("../data", 0777) == -1 && errno != EEXIST) {
        perror("Error al crear el directorio ../data");
        exit(1);
    }

    // Abrir el archivo en modo escritura de texto plano
    FILE *archivo = fopen(ruta_archivo, "w"); // "w" para texto
    if (archivo == NULL) {
        perror("Error al abrir el archivo");
        exit(1);
    }

    // Crear cuentas de ejemplo
    // Asegurarse que el titular no exceda los 49 caracteres para dejar espacio al null terminator
    Cuenta cuentas[] = {
        {1001, "Juan Vazquez", 1000.00f, 0},
        {1002, "Pedro Federico", 2000.67f, 0},
        {1003, "Maria Fernandez", 3000.43f, 0},
        {1004, "Ana Ramirez", 4000.23f, 0},
        {1005, "Carmen Denia", 5000.98f, 0},
        {1006, "Jose Luis Dominguez", 6000.50f, 0},
        {1007, "Gonzalo D'Lorenzo", 7000.75f, 0},
        {1008, "Fran Garcia", 8000.80f, 0},
        {1009, "Carlos Sevez", 9000.90f, 0}
    };
    
    // Calcular el número de cuentas
    size_t num_cuentas = sizeof(cuentas) / sizeof(cuentas[0]);
    
    // Escribir las cuentas en el archivo en formato texto: numero_cuenta|titular|saldo|bloqueado
    for (size_t i = 0; i < num_cuentas; i++) {
        // Asegurar que el campo titular esté terminado en null correctamente dentro de su tamaño
        cuentas[i].titular[sizeof(cuentas[i].titular) - 1] = '\0';
        if (fprintf(archivo, "%d|%s|%.2f|%d\n",
                    cuentas[i].numero_cuenta,
                    cuentas[i].titular,
                    cuentas[i].saldo,
                    cuentas[i].bloqueado) < 0) { // Usar fprintf para texto
            fprintf(stderr, "Error al escribir la cuenta %d en el archivo de texto.\n", cuentas[i].numero_cuenta);
            fclose(archivo);
            exit(1);
        }
    }

    // Cerrar el archivo
    fclose(archivo);
    printf("Cuentas guardadas exitosamente en formato texto plano en '%s'.\n", ruta_archivo);
    return 0;
}
