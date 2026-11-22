# Автозапуск Pi — завдання 10

Два незалежні **user systemd units** користувача `sokolovskyi`.
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
loginctl show-user sokolovskyi -p Linger
```

Для відео так само доступні start/stop/restart/enable/disable. Команди запускайте
як `sokolovskyi`, без sudo. Шлях units: `~/.config/systemd/user/`.

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
--port 14600 --sender 192.168.0.161 --timeout-ms 300`. CRSF 50 Hz задає наявний
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

Pipeline взято з історії команд Pi: MJPEG 640×480/30 → jpegdec → I420 →
openh264enc bitrate=2000000 complexity=low → h264parse → RTP PT96/MTU1200 → UDP.
Адреса одержувача відповідає завданню: `192.168.0.161:5602`.
Кодер та його параметри збережені. Додано `watchdog timeout=5000`, щоб відсутність
буферів завершувала pipeline для повторного запуску. Використано стабільний шлях
`/dev/v4l/by-id/usb-ARKMICRO_USB2.0_PC_CAMERA-video-index0` замість номера video0.
Перед запуском перевіряються пристрій і маршрут/стан мережевого інтерфейсу.
Помилка або EOS викликає повторний запуск через 3 с. UDP не потребує запущеного
одержувача; його відсутність не є помилкою відправника.

`FPV_CAMERA` дозволяє вказати інший device path через drop-in. Перевірка відсутнього
C2U використовувала неіснуючий шлях; фізичне USB unplug/replug і Wi-Fi off/on
окремо не виконувалися. Reboot перевіряється окремо у звіті.

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
SIGKILL/restart, незалежність, manual stop/start, відсутній C2U й відновлення.
Після завершення прибирає override і зупиняє RC. Не запускайте його на активному
фізичному стенді: він вимагає, щоб RC unit був disabled.
