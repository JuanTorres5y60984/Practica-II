# Practica-II

Este proyecto implementa un sistema bancario concurrente utilizando múltiples procesos y comunicación entre ellos. A continuación, se describen los componentes principales del sistema y cómo se integran.

## ANTES DE EMPEZAR
instalar xterm:  
```sh
sudo apt-get install xterm
```
compilar con: 
```sh
gcc -g -o bin/usuario src/usuario.c -pthread -lrt && gcc -g -o bin/monitor src/monitor.c -pthread -lrt && gcc -g -o bin/init_cuentas src/init_cuentas.c -pthread -lrt && gcc -g -o bin/banco src/banco.c -pthread -lrt
```
## Componentes

### 1. `banco.c`

Este programa es el núcleo del sistema bancario. Se encarga de leer la configuración, crear semáforos para controlar el acceso a los archivos y gestionar la comunicación con los usuarios.

- **Configuración:** Lee los parámetros de configuración desde un archivo.
- **Semáforos:** Utiliza semáforos para proteger las operaciones concurrentes en el archivo de cuentas.
- **Comunicación:** Crea tuberías y lanza procesos hijos para cada usuario. Redirige la salida estándar de los procesos hijos a las tuberías para leer las operaciones de los usuarios.

### 2. `usuario.c`

Este programa presenta un menú interactivo al usuario y envía las operaciones seleccionadas al proceso `banco.c` a través de la salida estándar.

- **Menú:** Presenta opciones para realizar depósitos, retiros, transferencias y consultar el saldo.
- **Operaciones:** Envía las operaciones seleccionadas a través de la salida estándar para que `banco.c` las procese.

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
gcc -o bin/banco src/banco.c -pthread -lrt
gcc -o bin/init_cuentas src/init_cuentas.c -pthread -lrt
gcc -o bin/monitor src/monitor.c -pthread -lrt
gcc -o bin/usuario src/usuario.c -pthread -lrt
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
