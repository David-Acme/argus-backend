AVISO DE PRIVACIDAD Y TÉRMINOS DE USO DE ARGUS (versión {{VERSION}})

Lee esto antes de instalar. Argus no se instala ni se configura hasta que lo
aceptes.

1. Dónde quedan tus datos
   Argus funciona por completo en este equipo. Las cámaras, los rostros, las
   voces, la agenda y las notificaciones se procesan y se guardan aquí, en tu
   hardware. Nada se envía a la nube. Si activas el acceso remoto, el túnel
   solo transporta datos cifrados entre tus dispositivos y este equipo.

2. Qué procesa Argus
   - Video y, si la cámara tiene micrófono, audio de las cámaras que agregues.
   - Rostros: para iniciar sesión (cada persona registra el suyo) y, solo si
     esa persona lo permite, para reconocerla en las cámaras.
   - Voz: las llamadas con Argus se transcriben en este equipo. Argus aprende
     a reconocer la voz de una persona solo si ella lo permite.
   - Presencia: si cada persona lo permite, Argus deduce si está en casa (por
     su conexión a la red de casa o por una cámara de entrada) para decidir a
     quién avisar primero ante una alarma. Guarda solo el estado actual (en
     casa / fuera), nunca la ubicación.
   - Notificaciones, llamadas de aviso, agenda y proyectos de la casa.

3. Datos sensibles
   El rostro y la voz son datos biométricos, que {{LAWS}} considera datos
   sensibles. Argus los guarda como patrones numéricos, no como grabaciones.
   La foto que cada persona toma al registrarse se guarda en el
   almacenamiento privado de este equipo y nunca se sincroniza a los
   teléfonos.

4. Cuánto tiempo se guardan
   - Argus no graba video de forma continua. Las imágenes de eventos de
     seguridad se guardan para revisarlas y se borran solas.
   - Rostros de desconocidos que pasan por las cámaras: 30 días sin volver a
     verlos.
   - Voz aprendida: muestras pendientes 30 días; muestras confirmadas 180
     días; se borra todo apenas la persona retira el permiso.
   - Presencia: solo el estado actual; se borra al retirar el permiso o tras
     30 días sin cambios.
   - En {{COUNTRY}}, la normativa pide conservar las grabaciones de
     videovigilancia {{VIDEO_DAYS}} días (máximo {{VIDEO_MAX_DAYS}}), y hasta
     {{INCIDENT_DAYS}} días cuando muestran un posible incidente o
     infracción. Revisa que la configuración de conservación de tu
     instalación respete esos plazos.

5. Cada persona decide
   Al entrar por primera vez a la app, cada persona elige qué permite:
   presencia, reconocimiento facial en cámaras, aprendizaje de voz y audio de
   las cámaras. Puede cambiarlo cuando quiera en Perfil > Privacidad. Sin su
   permiso, esas funciones quedan apagadas para ella. Como propietario puedes
   apagar una función para toda la casa, pero no encenderla por otra persona.

6. Tus responsabilidades
   - Cumplir las leyes de tu país sobre videovigilancia y datos de terceros.
     En {{COUNTRY}}: {{LAWS}}, supervisadas por la {{AUTHORITY}}.
   - Colocar avisos visibles de zona videovigilada donde haya cámaras.
   - No apuntar cámaras a espacios de terceros (la calle, casas vecinas) más
     de lo necesario.
   - Informar a quienes vivan, trabajen o visiten el lugar.

7. Cómo retirar tu aceptación
   Detén Argus (cd argus-deploy && docker compose down, sin -v) y ejecuta
   este mismo script con --withdraw-privacy-consent. Para borrar todos los
   datos, elimina la carpeta de datos de la instalación. Cada persona puede
   retirar sus permisos desde la app en cualquier momento.

8. Versión en desarrollo, sin garantías
   - Argus está en desarrollo (versión preliminar, antes de la beta) y se
     ofrece "tal cual", sin garantías de ningún tipo.
   - Su autor no se hace responsable del mal uso ni de fallas del software.
   - Argus no es un sistema de seguridad ni de alarma certificado. No
     reemplaza un servicio de monitoreo profesional ni los servicios de
     emergencia.
   - Eres responsable de cumplir las leyes locales sobre videovigilancia y
     datos de terceros, incluidos los avisos de zona videovigilada.

Este aviso no es asesoría legal.
