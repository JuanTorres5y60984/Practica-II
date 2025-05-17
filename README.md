# Practica-II

Este proyecto implementa un sistema bancario concurrente utilizando múltiples procesos y comunicación entre ellos. A continuación, se describen los componentes principales del sistema y cómo se integran.

## ANTES DE EMPEZAR
instalar xterm:  
```sh
sudo apt-get install xterm
```
compilar con: 
```sh
gcc -w -o bin/usuario src/usuario.c -pthread -lrt && gcc -w -o bin/monitor src/monitor.c -pthread -lrt && gcc -w -o bin/init_cuentas src/init_cuentas.c -pthread -lrt && gcc -w -o bin/banco src/banco.c -pthread -lrt
```
## Componentes

### 1. `banco.c`

Este programa es el núcleo del sistema bancario. Se encarga de:
- **Configuración:** Leer los parámetros de configuración desde `config/config.txt`.
- **Memoria Compartida:** Crear e inicializar un segmento de memoria compartida donde se almacenan los datos de todas las cuentas. Esta memoria es accesible tanto por el banco como por los procesos de usuario.
- **Sincronización:** Utilizar **mutexes (alojados en la memoria compartida)** para proteger el acceso concurrente a los datos de las cuentas y a un buffer circular de operaciones en la memoria compartida.
- **Gestión de Usuarios:**
    - Esperar la entrada del administrador (número de cuenta) para iniciar una sesión de usuario.
    - Crear **FIFOs (tuberías con nombre)** dedicados para la comunicación bidireccional con cada proceso de usuario.
    - Lanzar un nuevo proceso `usuario.c` (generalmente en una nueva ventana de `xterm`) para cada sesión.
- **Persistencia:** Un hilo dedicado en `banco.c` se encarga de escribir periódicamente (o cuando hay actividad) el estado de las cuentas desde la memoria compartida al archivo `cuentas.dat`, utilizando un buffer circular como mecanismo de señalización.
- **Logging:** Registrar eventos importantes en `data/transacciones.log`.

### 2. `usuario.c`
Este programa es la interfaz para el cliente final. Es lanzado por `banco.c`.
- **Menú Interactivo:** Presenta opciones para realizar depósitos, retiros, transferencias y consultar el saldo.
- **Comunicación con el Banco:**
    - Se comunica con el proceso `banco.c` a través de los **FIFOs** creados por el banco para enviar notificaciones de operaciones y recibir respuestas/confirmaciones.
- **Operaciones de Cuenta:**
    - Accede y **modifica directamente los datos de las cuentas (saldo, etc.) en la memoria compartida**, protegido por los mutexes definidos por el banco.
    - Notifica al banco sobre las operaciones realizadas para que puedan ser registradas y, eventualmente, persistidas a disco por el hilo de E/S del banco.
- **Logging:** Cada instancia de `usuario.c` crea un log individual en `/transacciones/<N_CUENTA>/transacciones.log`.


### 3. `init_cuentas.c`

Este programa inicializa el archivo de cuentas con datos de ejemplo.

- **Archivo de cuentas:** Lee la ruta del archivo de cuentas desde el archivo de configuración y escribe datos de ejemplo en él.

### 4. `monitor.c`

Este programa monitorea las transacciones y detecta patrones sospechosos.

- **Análisis de transacciones:** Lee las transacciones desde una cola de mensajes y analiza patrones sospechosos.
- **Alertas:** Envía alertas a través de una tubería si se detectan transacciones sospechosas.

## Configuración

El archivo de configuración (`config/config.txt`) contiene los siguientes parámetros:

- `LIMITE_RETIRO`: Límite máximo para retiros.
- `LIMITE_TRANSFERENCIA`: Límite máximo para transferencias.
- `UMBRAL_RETIROS`: Umbral para detectar retiros consecutivos sospechosos.
- `UMBRAL_TRANSFERENCIAS`: Umbral para detectar transferencias consecutivas sospechosas.
- `NUM_HILOS`: Número de hilos a utilizar.
- `ARCHIVO_CUENTAS`: Ruta del archivo de cuentas.
- `ARCHIVO_LOG`: Ruta del archivo de log.

## Ejecución

1. Compilar los programas:

```sh
gcc -w -o bin/usuario src/usuario.c -pthread -lrt && gcc -w -o bin/monitor src/monitor.c -pthread -lrt && gcc -w -o bin/init_cuentas src/init_cuentas.c -pthread -lrt && gcc -w -o bin/banco src/banco.c -pthread -lrt
```

2. Inicializar el archivo de cuentas:

```sh
./bin/init_cuentas
```

3. Ejecutar el programa `banco`:

```sh
sudo ./bin/banco
```

4. Ejecutar el programa `monitor` en otra terminal:

```sh
./bin/monitor
```

5. Los usuarios pueden interactuar con el sistema ejecutando el programa `usuario`:

```sh
introduciendo un numero de cuenta en el programa banco
```

## Notas

- Asegúrese de que los archivos de configuración y datos estén en las rutas correctas.
- Los semáforos se utilizan para proteger las operaciones concurrentes en el archivo de cuentas.
- El archivo de log registra todas las transacciones realizadas por los usuarios.
