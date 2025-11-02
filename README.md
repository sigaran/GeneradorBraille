# Generador Braille (ESP8266 / NodeMCU)

Proyecto que traduce texto en español a Braille de 6 puntos, accionando solenoides mediante **ULN2003A**.  
Incluye servidor web (modo **AP + STA**), envío por carácter o bloques de texto, botones **Test**, **Detener** y ajuste de **Pulso/Gap** en tiempo real.

---

## Características

- **Wi-Fi**: Punto de acceso (AP) propio y conexión a red (STA) en simultáneo.
- **Interfaz web** servida por el ESP8266 (no requiere Internet).
- **Traducción**:
  - Letras `a–z` (incluye **ñ/Ñ**).
  - **Mayúsculas** con prefijo (punto 6).
  - **Números** con prefijo **# (3456)** al inicio de secuencia (se cancela con separadores).
  - **Puntuación basica**: `, . ; : ¿ ? ¡ !`.
- **Ajustes en vivo**: duración del pulso y separación entre caracteres.
- **Acciones**: **Detener** (vacía cola y resetea estados) y **Test** (recorre puntos 1..6).

---

## Hardware

- **Microcontrolador**: NodeMCU v3 (ESP8266).
- **Driver**: ULN2003A (array de transistores con diodos).
- **Actuadores**: solenoides **12 V** (uno por punto).
- **Fuente**: 12 V para solenoides + USB 5 V para el ESP8266.

### Cableado

- Ejemplo de cableado con Tinkercad: **[Abrir simulación](<https://www.tinkercad.com/things/e79B1fsMXCp-pantalla-braille>)**
  - (Este proyecto utiliza arduino UNO y conjunto de diodos y transitores para simular ULN2003A, esto para fines de simular con codigo en Tinkercad

---

## Software

### Requisitos

- **Arduino IDE** (1.8.x o 2.x).
- **Driver USB** del convertidor de tu placa (CH340 o CP2102).
- **Core ESP8266**:
  1. IDE → **Preferencias** → “**URLs Adicionales de Tarjetas**” → agrega:  
     `http://arduino.esp8266.com/stable/package_esp8266com_index.json`
  2. IDE → **Herramientas → Placa → Gestor de tarjetas** → instala **“esp8266 by ESP8266 Community”**.

### Librerías

Incluidas en el core:
- `ESP8266WiFi.h`
- `ESP8266WebServer.h`
- `ESP8266mDNS.h`

No se requieren librerías externas adicionales.

---

## Compilación y carga

1. Abre el proyecto en Arduino IDE.
2. Selecciona placa/opciones (ver arriba).
3. Conecta el NodeMCU con cable **USB de datos**.
4. **Herramientas → Puerto**: elige el puerto correcto.
5. **Subir** el sketch.

---

## Uso

### Acceso por AP (punto de acceso)

- Conéctate al Wi-Fi:  
  **SSID**: `Generador-Braille` · **Clave**: `braille123`
- Abre `http://192.168.4.1/`

### Conexión a tu red (STA)

1. En la página principal pulsa **“Wi-Fi…”** o abre `http://192.168.4.1/wifi`.
2. **Buscar redes**, elige SSID, ingresa contraseña y **Conectar**.
3. Si conecta, verás la **IP STA** (p. ej., `http://192.168.1.50/`).
4. El AP permanece activo para reconfigurar.
5. También puedes probar `http://braille.local/` (si tu SO soporta mDNS).

### Interfaz web

- **Enviar todo**: envía el contenido del cuadro de texto.
- **Envío por carácter**: al escribir, se envía cada tecla.
- **Pulso (ms) / Espacio (ms)**: ajusta tiempos y pulsa **Aplicar**.
- **Detener**: vacía cola, apaga solenoides y resetea estados (incluye modo numérico).
- **Test**: recorre puntos 1..6 para validar hardware.
- **Wi-Fi…**: abre configuración STA.

---

## Rutas HTTP (para pruebas)

- `GET /` → UI principal (HTML)
- `GET /key?c=X` → encola carácter `X`
- `POST /text` (body `text/plain`) → encola texto completo
- `GET /cfg?pw=250&gap=40` → ajusta tiempos; devuelve JSON
- `GET /test` → test de puntos 1..6
- `GET /stop` → detener (limpia cola y resetea)
- `GET /ips` → muestra URLs AP/STA
- `GET /wifi` → UI Wi-Fi STA
- `GET /wifi_scan` → escanea redes (JSON)
- `GET /wifi_connect?ssid=...&pass=...` → conecta STA
- `GET /wifi_status` → estado e IPs (JSON)
- `GET /wifi_forget` → olvidar credenciales

---

## Mapeo Braille (6 puntos)

- **Letras**:  
- **ñ = 1-2-4-5-6**.
- **Mayúsculas**: prefijo **6** antes de la letra.
- **Números**: prefijo **3-4-5-6** al inicio de secuencia (`#123`); termina con espacio/puntuación.
- **Puntuación**:
  - `,` → **2**
  - `.` → **2-5-6**
  - `;` → **2-3**
  - `:` → **2-5**
  - `¡`/`!` → **2-3-5**
  - `¿`/`?` → **2-6**

> La decodificación UTF-8 está implementada para **ñ/Ñ/¿/¡**. Otros multibyte se omiten en esta demo.


