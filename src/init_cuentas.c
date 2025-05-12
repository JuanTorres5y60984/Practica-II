#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h> // Para mkdir
#include <errno.h>    // Para errno

// Definición de la estructura Cuenta
typedef struct {
    int numero_cuenta;
    char titular[50];
    float saldo;
    int num_transacciones;
    // El campo 'bloqueado' se añadirá al formato del archivo, inicializado a 0.
} Cuenta;

int main(void) {
    // Ruta del archivo de cuentas
    const char *ruta_archivo = "../data/cuentas.dat";
    
    // Crear directorio ../data/ si no existe
    if (mkdir("../data", 0777) == -1 && errno != EEXIST) {
        perror("Error al crear el directorio ../data");
        exit(1);
    }

    // Abrir el archivo en modo escritura
    FILE *archivo = fopen(ruta_archivo, "w");
    if (archivo == NULL) {
        perror("Error al abrir el archivo");
        exit(1);
    }
    
    // Crear cuentas de ejemplo
    Cuenta cuentas[] = {
        {1001, "Juan Vázquez", 1000.00f, 0},
        {1002, "Pedro Federico", 2000.67f, 0},
        {1003, "Maria Fernández", 3000.43f, 0},
        {1004, "Ana Ramírez", 4000.23f, 0},
        {1005, "Carmen Denia", 5000.98f, 0},
        {1006, "José Luis Dominguez", 6000.50f, 0},
        {1007, "Gonzalo D'Lorenzo", 7000.75f, 0},
        {1008, "Fran García", 8000.80f, 0},
        {1009, "Carlos Sévez", 9000.90f, 0}
    };
    
    // Calcular el número de cuentas
    size_t num_cuentas = sizeof(cuentas) / sizeof(cuentas[0]);
    
    // Escribir las cuentas en el archivo: numero_cuenta|titular|saldo|num_transacciones|bloqueado
    for (size_t i = 0; i < num_cuentas; i++) {
        fprintf(archivo, "%d|%s|%.2f|%d|%d\n", 
                cuentas[i].numero_cuenta, 
                cuentas[i].titular, 
                cuentas[i].saldo, 
                cuentas[i].num_transacciones,
                0); // 0 para indicar que la cuenta no está bloqueada
        if (ferror(archivo)) {
            perror("Error al escribir en el archivo");
            fclose(archivo);
            exit(1);
        }
    }
    
    // Cerrar el archivo
    fclose(archivo);
    printf("Cuentas guardadas exitosamente en '%s'.\n", ruta_archivo);
    return 0;
}
