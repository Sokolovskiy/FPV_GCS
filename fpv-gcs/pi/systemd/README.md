# Автозапуск Pi

Для встановлення на чисту Pi почніть із
[покрокової інструкції для Pi 3 Model B](https://github.com/Sokolovskiy/FPV_GCS/blob/main/docs/pi-setup.md).
Цей документ — довідка для вже налаштованих сервісів.

Два незалежні **user systemd units** користувача, який виконує встановлення
(у покроковій інструкції — `fpv`).
`loginctl enable-linger` запускає його systemd під час boot і зберігає після
закриття SSH. Обидва units мають `WantedBy=default.target`, `Restart=always`,
паузу 3 с і необмежені повторні спроби. Ручний stop не викликає restart.
Залежностей між RC та відео немає. Не створюйте дублікати system-level units.

## Щоденне керування на Pi

```sh
systemctl --user status fpv-video.service fpv-rc.service
systemctl --user stop fpv-rc.service
systemctl --user start fpv-rc.service
systemctl --user restart fpv-video.service
systemctl --user disable --now fpv-rc.service
systemctl --user enable --now fpv-rc.service
loginctl show-user "$USER" -p Linger
```

Для відео так само доступні start/stop/restart/enable/disable. Команди запускайте
як користувач, що встановив сервіси, без sudo. Шлях units: `~/.config/systemd/user/`.

## Логи

```sh
journalctl -b _SYSTEMD_USER_UNIT=fpv-video.service -f
journalctl -b _SYSTEMD_USER_UNIT=fpv-rc.service -f
```

Ці команди перевірені на Pi. `journalctl --user -u ...` у поточній конфігурації
journald повертає порожній журнал; потрібний наведений фільтр системного журналу.
Користувач має групу adm. Збереження journal між reboot не налаштовувалося.

## RC

Запускається наявний `~/fpv-gcs-udp/rc_receiver.py`: `--uart --baudrate 420000
--port 14600 --sender 192.168.144.3 --timeout-ms 300`. CRSF 50 Hz задає наявний
encoder. RC-файли, мапінг і правила recovery не змінені. Після кожного запуску
новий процес починає з FAILSAFE, 0 кадрів; потрібно >=10 послідовних валідних
пакетів, >=160 мс, інтервали <=60 мс. Немає відтворення команд з диска чи ARM API.
Поточні AUX передаються за наявними правилами; сервіс не примушує ARM до OFF.

Фізичний unit додатково вимагає файл
`~/.config/fpv-gcs/physical-crsf-approved`. Його створено лише після явного
підтвердження оператора: пропелери зняті, ARM OFF, антена ES900TX підключена.
Це постійний дозвіл для наступних boot; для відкликання спочатку виконайте
`systemctl --user disable --now fpv-rc.service`, потім видаліть файл.

## Відео

Відеосервіс запускає `../video_capture.py` через наявний `launch.py video`.
Менеджер тримає один GStreamer pipeline і той самий вихід
`H.264 / RTP / UDP → 192.168.144.3:5602` (PT96, MTU1200, OpenH264).
Початковий capture format читається з пристрою; збережений формат має пріоритет.
Помилки pipeline повторюються через 3 с, без перезапуску RC чи всього gateway.

Менеджер опитує USB V4L2-пристрої кожні 2 с. Серійний `by-id` має пріоритет;
для захватів без серійного номера використовується `by-path` (той самий USB-порт).
Metadata nodes та не-USB кодери Pi не потрапляють до селектора. Порядок
детермінований: сортування стабільних ID. Preferred camera відновлюється при
старті; якщо її немає, береться перша доступна. Наявний активний USB-пристрій
не змінюється через втрату CVBS або появу іншого dongle. Без камер сервіс і API
залишаються активними й чекають підключення.

Залежності на Pi (один раз):

```sh
sudo apt-get install python3-gi gir1.2-gstreamer-1.0 v4l-utils psmisc
```

Потрібні вже наявні GStreamer plugins: video4linux2, queue, videoconvert,
jpegdec, openh264enc, h264parse, watchdog, rtph264pay, udpsink.
Оновіть `~/fpv-gcs-udp/video_capture.py` та `systemd/launch.py`, потім виконайте
`systemctl --user restart fpv-video.service`. RC restart не потрібний.
`FPV_CAMERA` більше не використовується: джерелом керує менеджер.

API слухає `192.168.144.20:5603`, приймає запити лише від `192.168.144.3`
та самого Pi і відхиляє browser Origin. Це API виділеного довіреного відеолінка.
`FPV_VIDEO_API_BIND` / `FPV_VIDEO_API_PORT` змінюють bind відеоменеджера;
Windows читає `FPV_VIDEO_API_URL` (типово `http://192.168.144.20:5603`).
Адреса самого RTP потоку не змінюється цими параметрами.

`GET /state` повертає кешований стан. `POST /select`, `/refresh`, `/controls`,
`/reset`, `/configure` ставлять операцію в чергу та повертають `operation`.
Її результат доступний у `state.operations[id]`. Для редагування налаштувань
передається `device` зі стабільним ID, щоб запізнілий запит не змінив іншу камеру.
Зайнятий USB-пристрій не забирається в іншого процесу.

Конфігурація: `~/.config/fpv-gcs/video-capture.json`, атомарний запис,
`preferred` окремо від per-device `format`, `controls`, `encoding`, input/standard.
Меню, діапазони та значення читаються з V4L2/GStreamer. Image controls
застосовуються наживо; якщо драйвер відмовляє, виконується зупинка лише
відеоконвеєра. Format/encoding застосовуються з контрольованим restart і
відкатом до попередньої конфігурації при помилці. Reset стосується image controls
із доступним driver default. Непідтримувані PAL/NTSC поля не показуються.
Показник RTP bitrate вимірює bytes-served udpsink; dropped frames — лічильник
transport sink, без припущень про втрати поза Pi.

Перевірки без пристроїв: `cd ~/fpv-gcs-udp && python3 -m unittest test_video_capture -v`.
Фізичне unplug/replug та boot Pi перевіряються окремо; сервісний restart не є boot.

Перевірено на стенді 2026-11-24: MACROSILICON AFN_Cap video (0002:afa1,
позначений оператором як MS2107) та ARKMICRO USB2.0 PC CAMERA (18ec:5555).
На Windows декодовано реальні H.264 кадри після вибору кожного джерела,
зміни bitrate 2.0 → 2.1 → 2.0 Mbps та перемикання YUYV 720×576/25 ↔
MJPEG 640×480/30 на MACROSILICON. Обидва формати окремо відновлено після
service restart. Яскравість змінено й повернуто на обох захватах без зміни
pipeline generation. Перевірено preferred ARKMICRO після restart і спільний
стан dropdown у toolbar/modal, зміну controls при перемиканні камер та LIVE
після повернення до MACROSILICON. RC увесь час мав той самий MainPID=845,
NRestarts=0. Фізичне unplug/replug та reboot не виконувалися.

## Один власник пристрою

Обгортка `launch.py` тримає окремі flock для RC та C2U протягом життя процесу,
перевіряє зайнятість через fuser і відмовляється забирати пристрій у старого
процесу. UART також використовує наявний pyserial exclusive lock. Користуйтеся
systemctl для ручного запуску; перед прямим запуском старих скриптів зупиняйте
відповідний сервіс. Другий запуск через обгортку перевірено: відхиляється.

## Встановлення і перевірки

Файли цього каталогу розміщені у `~/fpv-gcs-udp/systemd/`.
`python3 systemd/install.py` встановлює units та вмикає відео/linger;
вперше він не вмикає RC. `verify_services.py` призначений лише для етапу ДО
увімкнення фізичного RC: використовує тимчасовий dry-run override, перевіряє
SIGKILL/restart, незалежність, manual stop/start та відновлення відеоменеджера і preferred device.
Після завершення прибирає override і зупиняє RC. Не запускайте його на активному
фізичному стенді: він вимагає, щоб RC unit був disabled.
