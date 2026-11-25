# Raspberry Pi 3 Model B: встановлення FPV GCS з нуля

Ця інструкція починається з порожньої microSD-карти. Монітор і клавіатура для
Pi не потрібні: налаштуємо її з Windows через домашній Wi-Fi, а потім підключимо
Ethernet для FPV GCS. На Pi встановлюємо Raspberry Pi OS та програми з релізу
0.1.10. Окремого готового образу microSD проєкт поки не має.

**Межа перевірки:** кроки звірено з кодом проєкту й офіційною документацією.
Повне встановлення на чисту Pi 3 Model B та її продуктивність із відео ще
не перевірені на платі. Успішне встановлення пакетів не означає, що Pi витримає
потрібну частоту кадрів; перевірка навантаження є наприкінці.

## Як читати інструкцію

- **На Windows** — команда у PowerShell вашого ПК.
- **На Pi** — команда в тому самому вікні, але після входу через `ssh`.
- Вставляйте блок команди повністю й натискайте Enter. Дочекайтеся завершення.
- Якщо з'явилася помилка, зупиніться на цьому кроці; не вставляйте наступні команди.
- Пароль у терміналі не показується навіть зірочками. Введіть його та натисніть Enter.
- `~` і `$HOME` означають домашню папку користувача Pi.

## 1. Приготуйте обладнання

| Що потрібно | Для чого |
| --- | --- |
| Raspberry Pi **3 Model B** | Комп'ютер ретранслятора |
| microSD 16 ГБ або більше та кардридер | На карті будуть ОС і програми |
| Якісне живлення **5,1 В / 2,5 А, micro-USB** | Живлення Pi; USB-захват теж споживає струм |
| Windows-ПК з Wi-Fi та Ethernet | Встановлення і FPV GCS |
| Домашня Wi-Fi мережа **2,4 ГГц** з Інтернетом | Початковий доступ до Pi та завантаження пакетів |
| Ethernet-кабель | Перша перевірка ПК ↔ Pi без Herelink |
| USB-відеозахват C2U/UVC, VRX та джерело аналогового відео | Перевірка зображення |
| Пульт з USB Joystick, ES900TX та приймач | Подальша перевірка керування |

Pi 3 Model B має 1 ГБ RAM і Wi-Fi лише 2,4 ГГц. Вибір Lite нижче зменшує
витрати пам'яті на робочий стіл. Живлення й можливості плати описані в
[документації живлення](https://www.raspberrypi.com/documentation/computers/raspberry-pi.html#power-supply)
та [огляді плат Raspberry Pi](https://www.raspberrypi.com/news/raspberry-pi-product-series-explained/).

## 2. Запишіть операційну систему на microSD

**На Windows:**

1. Завантажте й встановіть [Raspberry Pi Imager](https://www.raspberrypi.com/software/).
2. Вставте microSD у кардридер ПК.
3. Запустіть Imager. Виберіть пристрій **Raspberry Pi 3**.
4. Виберіть **Raspberry Pi OS Lite (64-bit)** у списку Raspberry Pi OS.
   Якщо потрібно, відкрийте категорію **Other**. Lite не має робочого столу — це нормально.
   Для цієї інструкції потрібна сучасна Raspberry Pi OS із NetworkManager;
   старі образи Legacy/Bullseye не використовуйте.
5. Виберіть саме свою microSD за розміром. **Запис зітре всі дані на вибраній карті.**
6. У персоналізації ОС задайте:

   | Поле | Значення |
   | --- | --- |
   | Hostname / ім'я пристрою | `fpv-pi` |
   | Username / користувач | `fpv` |
   | Password / пароль | Придумайте власний і збережіть його |
   | Wi-Fi | Назва та пароль вашої мережі **2,4 ГГц** |
   | Wireless country | Країна фактичного використання |
   | Time zone | Ваш часовий пояс |
   | SSH / Remote access | Увімкнути; вхід за паролем |

7. Почніть запис, застосувавши ці налаштування. Дочекайтеся і запису, і перевірки.
8. Вийміть карту. Якщо Windows пропонує форматувати розділ карти, натисніть **Скасувати**.

Підтримка 64-bit для Pi 3 описана в
[Raspberry Pi OS](https://www.raspberrypi.com/documentation/computers/os.html),
а поля налаштування — в [інструкції Imager](https://www.raspberrypi.com/documentation/computers/imager.html).
Назви екранів можуть трохи відрізнятися між версіями Imager.

## 3. Увімкніть Pi та підключіться до неї

1. Вставте microSD у **вимкнену** Pi.
2. Поки не підключайте ES900TX до GPIO.
3. Подайте живлення через micro-USB. Перший запуск може тривати кілька хвилин.
4. ПК і Pi повинні бути в одній домашній мережі; гостьова мережа з ізоляцією пристроїв не підходить.
5. На Windows відкрийте **Пуск → PowerShell** і введіть:

```powershell
ssh fpv@fpv-pi.local
```

Під час першого підключення SSH попросить підтвердити ключ пристрою.
Переконайтеся, що це ваша щойно ввімкнена Pi, введіть `yes`, потім пароль із кроку 2.
Після входу побачите запрошення на кшталт `fpv@fpv-pi:~ $`.
**Тепер наступні команди виконуються на Pi.**

Якщо ім'я `fpv-pi.local` не знайдено, відкрийте список підключених пристроїв
вашого роутера, знайдіть `fpv-pi` та її IP. Наприклад, для IP `192.168.1.50`
команда буде `ssh fpv@192.168.1.50`. Це приклад: використайте адресу свого роутера.
Якщо `ssh` не знайдено на Windows, встановіть компонент **OpenSSH Client**
у додаткових компонентах Windows. Подробиці — у
[довідці SSH Raspberry Pi](https://www.raspberrypi.com/documentation/computers/remote-access.html#ssh).

## 4. Встановіть усі залежності

**На Pi**, спочатку оновіть список пакетів:

```sh
sudo apt update
```

Встановіть залежності одним блоком:

```sh
sudo apt install -y python3 python3-serial python3-gi python3-gst-1.0 gir1.2-gstreamer-1.0 gir1.2-gst-plugins-base-1.0 gstreamer1.0-tools gstreamer1.0-plugins-base gstreamer1.0-plugins-good gstreamer1.0-plugins-bad v4l-utils psmisc curl unzip avahi-daemon
```

Використовуємо системний Python і пакети `apt`; окреме `pip install` чи `.venv`
для gateway не потрібні. Залежність `pymavlink` у корені репозиторію стосується
окремого MAVLink-монітора, цей gateway її не використовує.

Перевірте Python:

```sh
python3 -c 'import serial, gi; gi.require_version("Gst", "1.0"); from gi.repository import Gst; Gst.init(None); print("Python та GStreamer: OK")'
```

Перевірте потрібні відеокомпоненти:

```sh
for element in v4l2src queue videoconvert jpegdec openh264enc h264parse watchdog rtph264pay udpsink; do
    if gst-inspect-1.0 "$element" >/dev/null 2>&1; then
        echo "OK: $element"
    else
        echo "MISSING: $element"
    fi
done
```

**Результат:** рядок `Python та GStreamer: OK` і дев'ять рядків `OK:`.
Якщо є `MISSING`, не продовжуйте: скопіюйте назву компонента та помилку встановлення.
Склад пакетів можна звірити з
[Python GStreamer](https://packages.debian.org/trixie/python3-gst-1.0)
та [GStreamer bad plugins / OpenH264](https://packages.debian.org/trixie/gstreamer1.0-plugins-bad).

## 5. Завантажте пакет FPV GCS для Pi

**На Pi**, завантажте ZIP і контрольні суми з релізу 0.1.10:

```sh
mkdir -p "$HOME/fpv-downloads"
cd "$HOME/fpv-downloads"
curl -fL --retry 3 -o FPV-GCS-Pi-0.1.10.zip https://github.com/Sokolovskiy/FPV_GCS/releases/download/v0.1.10/FPV-GCS-Pi-0.1.10.zip
curl -fL --retry 3 -o SHA256SUMS-0.1.10.txt https://github.com/Sokolovskiy/FPV_GCS/releases/download/v0.1.10/SHA256SUMS-0.1.10.txt
sha256sum --check --ignore-missing SHA256SUMS-0.1.10.txt
```

**Результат:** `FPV-GCS-Pi-0.1.10.zip: OK`. Windows-пакети не завантажені,
тому `--ignore-missing` пропускає їх. Якщо ZIP не пройшов перевірку, не розпаковуйте його.

Розпакуйте:

```sh
unzip FPV-GCS-Pi-0.1.10.zip -d "$HOME/fpv-downloads/unpacked-0.1.10"
```

Архів містить папку `pi`. Перевірте, що цільова папка ще не існує:

```sh
ls -ld "$HOME/fpv-gcs-udp"
```

**На чистій установці очікується** `No such file or directory` — тут це нормально.
Якщо папка вже є, зупиніться: це вже процедура оновлення, а не чиста установка.
Якщо папки немає, перенесіть розпаковану `pi`:

```sh
mv "$HOME/fpv-downloads/unpacked-0.1.10/pi" "$HOME/fpv-gcs-udp"
cd "$HOME/fpv-gcs-udp"
ls rc_receiver.py video_capture.py systemd/install.py
```

**Результат:** видно всі три файли. Не повинно бути зайвого вкладення
`~/fpv-gcs-udp/pi/` — сервіси шукають скрипти безпосередньо в `~/fpv-gcs-udp/`.

Запустіть програмні перевірки без підключення UART:

```sh
python3 -m unittest -q
```

В останньому рядку має бути `OK` (можлива позначка пропущених перевірок).
Якщо є `FAILED` чи traceback, збережіть вивід і не переходьте до фізичного RC.

## 6. Задайте адресу Ethernet на Pi

Домашній Wi-Fi залишаємо для Інтернету та SSH. Через Ethernet піде зв'язок FPV.
Для першої перевірки підключіть Ethernet Pi **прямо до Ethernet Windows-ПК**;
Herelink додамо після успішної перевірки цього з'єднання.

**На Pi**, перевірте імена інтерфейсів:

```sh
nmcli device status
```

Має бути `eth0`. Якщо його немає, спочатку з'ясуйте назву Ethernet-порту.
Для чистої установки з `eth0` виконайте:

```sh
sudo nmcli connection add type ethernet ifname eth0 con-name fpv-ethernet ipv4.method manual ipv4.addresses 192.168.144.20/24 ipv4.never-default yes ipv6.method disabled connection.autoconnect yes connection.autoconnect-priority 100
sudo nmcli connection up fpv-ethernet
ip -4 address show eth0
```

**Результат:** вивід містить `192.168.144.20/24`. Налаштування збережеться після
перезапуску. Команду `connection add` виконуйте один раз; для повторного ввімкнення
достатньо `sudo nmcli connection up fpv-ethernet`.

Якщо немає Ethernet-кабелю/лінка, `connection up` може завершитися помилкою:
підключіть кабель і повторіть лише цю команду. Поки немає адреси `.20`, відеосервіс
не зможе прив'язати API. `ipv4.never-default yes` зберігає Інтернет через Wi-Fi.
Довідка: [nmcli](https://www.networkmanager.dev/docs/api/latest/nmcli.html),
[IPv4](https://www.networkmanager.dev/docs/api/latest/settings-ipv4.html).

## 7. Задайте адресу Ethernet на Windows

**На Windows**, відкрийте друге вікно PowerShell; перше з SSH залиште відкритим.

1. Натисніть `Win+R`, введіть `ncpa.cpl`, натисніть Enter.
2. Знайдіть Ethernet-адаптер із кабелем до Pi. Не змінюйте Wi-Fi.
3. Запишіть його попередні IPv4-налаштування, щоб потім їх можна було повернути.
4. Відкрийте **Властивості → IP версії 4 (TCP/IPv4) → Властивості**.
5. Виберіть ручну IP-адресу та введіть:

   | Поле | Значення |
   | --- | --- |
   | IP-адреса | `192.168.144.3` |
   | Маска | `255.255.255.0` |
   | Основний шлюз | Залишити порожнім |
   | DNS | Залишити порожнім для цього адаптера |

6. Збережіть. Переконайтеся, що домашня мережа/VPN не використовує цю саму підмережу.
7. У **другому, локальному PowerShell** перевірте:

```powershell
ping 192.168.144.20
```

**Результат:** відповіді від `192.168.144.20`. Після цього можна входити на Pi
кабелем командою `ssh fpv@192.168.144.20`, навіть без домашнього Wi-Fi.

## 8. Підготуйте UART для CRSF — особливо важливо на Pi 3

UART — два сигнальні контакти TX/RX. Для передавання CRSF потрібен TX.
На Pi 3 звільнимо повноцінний UART **PL011** від Bluetooth. Bluetooth після цього
буде вимкнений; Wi-Fi залишається доступним.

**На Pi:**

```sh
sudo raspi-config
```

Стрілками й Enter відкрийте **Interface Options → Serial Port**:

1. На запит про **login shell over serial** виберіть **No**.
2. На запит про **serial port hardware** виберіть **Yes**.
3. Виберіть **Finish**, але поки відкладіть пропозицію reboot.

Збережіть копію конфігурації та відкрийте її:

```sh
sudo cp -n /boot/firmware/config.txt /boot/firmware/config.txt.before-fpv
sudo nano /boot/firmware/config.txt
```

У самому кінці додайте окремими рядками:

```ini
[all]
enable_uart=1
dtoverlay=disable-bt
```

Збережіть: **Ctrl+O → Enter → Ctrl+X**. Далі:

```sh
sudo systemctl disable --now hciuart.service
sudo usermod -aG dialout,video,adm "$USER"
sudo reboot
```

Якщо саме `hciuart.service` відсутній, це означає, що службу не встановлено;
переконайтеся, що `dtoverlay=disable-bt` додано, і виконайте наступні команди.
SSH розірветься через перезапуск. Через кілька хвилин **на Windows** увійдіть знову:

```powershell
ssh fpv@192.168.144.20
```

**На Pi**, перевірте результат:

```sh
readlink -f /dev/serial0
groups
cat /proc/cmdline
```

Очікується `/dev/ttyAMA0`, а в групах — `dialout`, `video`, `adm`.
У `/proc/cmdline` не повинно бути `console=serial0,...`, `console=ttyAMA0,...`
або `console=ttyS0,...`. `console=tty1` залишати можна.
Якщо бачите `/dev/ttyS0` чи серійну консоль, не переходьте до RC — повторно
перевірте налаштування цього кроку.

Джерело налаштування UART і `disable-bt`:
[офіційна документація Raspberry Pi](https://www.raspberrypi.com/documentation/computers/configuration.html#configure-uarts).

## 9. Установіть автозапуск і перевірте відео

Підключіть C2U до USB Pi та подайте на нього відео від VRX.
**На Pi** перевірте, що захват видно:

```sh
v4l2-ctl --list-devices
```

Має бути ваш USB-захват і шлях на кшталт `/dev/video0`. Номер може відрізнятися.
Потім установіть сервіси від звичайного користувача `fpv`:

```sh
sudo loginctl enable-linger "$USER"
cd "$HOME/fpv-gcs-udp"
python3 systemd/install.py
systemctl --user status fpv-video.service --no-pager
loginctl show-user "$USER" -p Linger
curl -f http://127.0.0.1:5603/state
```

**Результат:** `active (running)`, `Linger=yes`, а остання команда повертає JSON
із полями `devices`, `stream_state`, `error`. Без камери сервіс може працювати
й чекати на пристрій; це ще не означає, що зображення передається.
Не запускайте інсталятор через `sudo python3`: це встановить сервіси іншому користувачу.
RC цей інсталятор уперше **не вмикає**.

На Windows установіть [FPV GCS 0.1.10](https://github.com/Sokolovskiy/FPV_GCS/releases/tag/v0.1.10)
та запустіть його. Дозвольте програмі доступ у брандмауері для мережі з Pi.
Потрібні вхідне UDP-відео `5602` на ПК та доступ ПК до TCP `5603` на Pi.
Якщо правила створюються вручну, обмежуйте їх адресою Pi `192.168.144.20`.

У FPV GCS виберіть USB-камеру. Після появи реального відеосигналу має з'явитися
зображення. У **Video Capture Settings** перевірте формат і FPS.
Код використовує програмний `openh264enc`, тому для першої перевірки Pi 3
оберіть менший **підтримуваний захватом** режим, наприклад `640×480, 15 FPS`,
якщо він є в списку. Не вписуйте режим, якого пристрій не підтримує.

## 10. Перевірте RC без сигналу на GPIO

Поки ES900TX ще не підключено до GPIO, підключіть USB-пульт до Windows і
перевірте його канали в FPV GCS. Для темплейта дивіться
[інструкцію TORO HERELINK](https://github.com/Sokolovskiy/FPV_GCS/blob/main/docs/radio-template.md).

В FPV GCS адреса RC має бути `192.168.144.20`, порт — `14600`.
**На Pi** запустіть безпечну програмну перевірку:

```sh
cd "$HOME/fpv-gcs-udp"
python3 -u systemd/launch.py rc-dry-run
```

Має з'явитися `CRSF DRY-RUN memory only` і `UART never opened`.
Коли надходить стабільний потік каналів, стан переходить до `ACTIVE`.
Порухайте органи пульта й перевірте канали. Від'єднайте USB-пульт від ПК:
після тайм-ауту стан має стати `FAILSAFE`. Підключіть пульт назад і перевірте
повторне відновлення стабільного потоку.

Для завершення натисніть **Ctrl+C**. Цей процес не запускається автоматично
і не передає нічого на UART. Не запускайте його одночасно з фізичним RC-сервісом.

## 11. Підключіть ES900TX і дозвольте фізичний RC

Цей крок виконуйте на стенді зі знятими пропелерами, ARM OFF та підключеною
антеною ES900TX. Живлення й пайку нехай перевірить людина з досвідом електроніки.

1. Вимкніть Pi командою `sudo poweroff`, дочекайтеся завершення й від'єднайте живлення.
2. За [схемою підключення](https://github.com/Sokolovskiy/FPV_GCS/blob/main/docs/connections.md)
   з'єднайте Pi **фізичний pin 8 / GPIO14 TX** з **CRSF IN** ES900TX,
   а **фізичний pin 6 / GND** — із землею ES900TX.
3. ES900TX живіть відповідно до документації його ревізії. GPIO — логіка 3,3 В;
   не підводьте 5 В до сигнального контакту Pi. Контакт живлення модуля не є CRSF IN.
4. Увімкніть Pi, ES900TX і вже спарений приймач. Знову увійдіть через SSH.
5. Лише після перевірки стенда **на Pi** виконайте:

```sh
mkdir -p "$HOME/.config/fpv-gcs"
touch "$HOME/.config/fpv-gcs/physical-crsf-approved"
systemctl --user enable --now fpv-rc.service
systemctl --user status fpv-rc.service --no-pager
```

Файл `physical-crsf-approved` зберігає дозвіл і на наступні ввімкнення Pi.
CRSF передається через `/dev/serial0` зі швидкістю `420000 baud`, `50 Гц`;
приймачем UDP дозволено саме Windows `192.168.144.3`.
Перевірте реальні канали на FC, мінімальний газ, напрямки та disarm/ARM.
Перевірте failsafe при втраті зв'язку, залишаючи пропелери знятими.
Налаштування приймача, binding, AUX і failsafe польотного контролера не задаються цим інсталятором.

Щоб зупинити RC зараз:

```sh
systemctl --user stop fpv-rc.service
```

Щоб заборонити автозапуск RC та відкликати збережений дозвіл:

```sh
systemctl --user disable --now fpv-rc.service
rm -f "$HOME/.config/fpv-gcs/physical-crsf-approved"
```

## 12. Поставте Herelink замість прямого Ethernet-кабелю

Після успішної прямої перевірки підключіть:

**ПК Ethernet → Herelink Ground ↔ Herelink Air → Pi Ethernet.**

Блоки Herelink мають бути вже спарені й працювати як мережевий міст.
Адреси ПК `.3` і Pi `.20` не змінюйте. Власні адреси блоків Herelink перевірте
за їх налаштуваннями; вони не повинні збігатися з ПК/Pi.
Повторіть `ping 192.168.144.20` з Windows, перевірку відео й каналів.
Якщо напряму все працює, а через Herelink ні, перевіряйте міст, кабелі та живлення блоків.

## 13. Перевірте перезапуск і навантаження Pi 3

На стенді, залишаючи пропелери знятими, **на Pi** виконайте:

```sh
sudo reboot
```

Після завантаження знову зайдіть через SSH і перевірте:

```sh
systemctl --user is-active fpv-video.service fpv-rc.service
loginctl show-user "$USER" -p Linger
vcgencmd get_throttled
vcgencmd measure_temp
top
```

Якщо крок 11 завершено, очікуються два рядки `active`. До дозволу фізичного RC
його `inactive` є нормальним. Відео й канали потрібно перевірити ще раз після boot.
`throttled=0x0` означає відсутність поточних і зафіксованих цим індикатором
проблем живлення/обмеження частоти; це не вимір затримки відео.
Вихід із `top` — клавіша **q**.

Якщо CPU постійно завантажений, Pi перегрівається або відео відстає, зменшіть
підтримувані роздільність/FPS у налаштуваннях захвату й повторіть перевірку.
Зменшення bitrate саме по собі не гарантує зменшення обчислювального навантаження.
Можливість одночасної роботи RC та потрібного відеорежиму на Pi 3 Model B
потрібно підтвердити на вашій платі до використання.

Для коректного вимкнення використовуйте `sudo poweroff`, дочекайтеся завершення
й лише потім від'єднуйте живлення.

## Якщо щось не працює

| Симптом | Що перевірити |
| --- | --- |
| Не знаходить `fpv-pi.local` | Живлення, Wi-Fi 2,4 ГГц, пароль, SSH у Imager; знайдіть IP Pi в роутері |
| Відмовлено у вході | Користувач `fpv` та власний пароль із Imager; користувач `pi` автоматично не створюється |
| Pi відповідає через Wi-Fi, але не на `.20` | Кабель, `ip -4 address show eth0`, профіль `fpv-ethernet`, IPv4 Ethernet ПК |
| `Cannot assign requested address` | На Pi ще немає адреси `192.168.144.20` на активному Ethernet |
| `No element openh264enc` / інший `MISSING` | Установіть пакети кроку 4 й повторіть перевірку компонентів |
| Немає `/dev/video*` | Кабель USB, живлення, `v4l2-ctl --list-devices`, сумісність захвату |
| Камера є, зображення немає | Аналоговий сигнал/VRX, формат камери, IP ПК, UDP 5602 і брандмауер |
| API повертає `403` | Звертайтеся з ПК через Ethernet `.3` або локально на Pi через `127.0.0.1`; браузер із Origin відхиляється |
| `Permission denied` для UART/камери | Групи `dialout`/`video` й перезапуск після `usermod` |
| RC unit пропущений за condition | Крок 11: маркер дозволу ще не створено; не створюйте його до перевірки стенда |
| `Refusing busy/uncheckable device` | UART зайнятий; перевірте серійну консоль і дублікати процесів. Не вбивайте невідомі процеси навмання |
| Сервіс постійно перезапускається | Прочитайте останні логи командами нижче; `active` саме по собі не доводить працездатність |
| Помилка встановлення/перезапису unit | Не перезаписуйте чинний unit навмання: інструкція призначена для чистої установки |

**На Pi**, отримати останні логи:

```sh
journalctl -b _SYSTEMD_USER_UNIT=fpv-video.service -n 60 --no-pager
journalctl -b _SYSTEMD_USER_UNIT=fpv-rc.service -n 60 --no-pager
```

Для допомоги надішліть текст помилки, вивід цих команд та відомості про ОС:

```sh
cat /etc/os-release
uname -m
cat /proc/device-tree/model
```

Паролі Wi-Fi та SSH надсилати не потрібно.

## Коли встановлення завершено

- [ ] Pi доступна за `192.168.144.20` через Ethernet після reboot.
- [ ] У FPV GCS видно реальне відео та всі потрібні канали.
- [ ] Перевірено фізичний RC і failsafe на стенді.
- [ ] Після reboot сервіси запускаються без відкритого SSH.
- [ ] Перевірено роботу через Herelink.
- [ ] Для вибраного відеорежиму на Pi 3 перевірено навантаження й затримку.

Додатково: [керування сервісами](https://github.com/Sokolovskiy/FPV_GCS/blob/main/fpv-gcs/pi/systemd/README.md),
[протокол RC та watchdog](https://github.com/Sokolovskiy/FPV_GCS/blob/main/fpv-gcs/pi/README.md).
