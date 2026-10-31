# Yale BLE → HomeKit på ESP32

ESP32 ansluter lokalt till en Yale Access Module via Bluetooth och visar låset i Apple Hem via HomeKit. BLE-protokollet bygger på [yalexs-ble](https://github.com/Yale-Libs/yalexs-ble); Python-paketet körs inte på ESP32.

## Du behöver

- ESP32 med BLE, 2,4 GHz Wi‑Fi och minst 4 MB flash (testad med ESP32-D0WD-V3).
- Ett kompatibelt Yale/August-lås med FE24 BLE-tjänsten och din egen **offline key** och **slot**. [Så hämtar du uppgifterna](docs/yale-offline-key.md).
- Apple Hem på iPhone/iPad och ett nät där telefon och ESP32 kan nå varandra.

## Installera via USB första gången

1. Installera [Arduino IDE 2](https://www.arduino.cc/en/software), [Arduino-ESP32 3.3.12](https://docs.espressif.com/projects/arduino-esp32/en/latest/installing.html), [HomeSpan 2.1.8](https://github.com/HomeSpan/HomeSpan) och [NimBLE-Arduino 2.5.2](https://github.com/h2zero/NimBLE-Arduino).
2. Öppna `YaleBleHomeKit/YaleBleHomeKit.ino`. Välj **ESP32 Dev Module** och **Tools → Partition Scheme → No FS 4MB (2MB APP x2)** (`no_fs`). Det viktiga är två app-partitioner på cirka 1,9 MB vardera.
3. Välj rätt USB-port och tryck **Upload**. Serial Monitor använder 115200 baud.

Har enheten redan flashats med version 0.1.0 och **Huge APP** behövs denna USB-flashning en gång. Efteråt fungerar GitHub-uppdatering via adminvyn. Inställningar ligger i NVS och ska följa med vid byte till `no_fs` på en 4 MB ESP32, men spara din offline key och ditt adminlösenord före flashning.

## Första start

1. Anslut telefonen till det öppna Wi‑Fi-nätet **Yale-BLE-Setup-XXXXXX**. Öppna `http://192.168.4.1` om portalen inte visas automatiskt.
2. Välj ditt 2,4 GHz Wi‑Fi, ange nätets lösenord och skapa ett adminlösenord på minst 12 tecken. Spara HomeKit-koden som visas.
3. När ESP32 har startat om, öppna `http://yale-ble.local` eller enhetens IP-adress på hemnätet och logga in.
4. Klicka **Sök BLE-enheter**, välj låset och spara. Ange sedan **offline key** som 32 hextecken och **key index / slot** som ett tal 0–255. [Fullständig guide för dessa uppgifter](docs/yale-offline-key.md). BLE-namnet hjälper ESP32 hitta låset om adressen ändras.
5. Lägg till **Yale BLE** som tillbehör i Apple Hem med koden från steg 2.

Adminvyn visar anslutning, låsstatus och senaste fel. Upplåsning görs i Apple Hem.

## Uppdatera firmware från GitHub

1. Publicera projektet som ett **offentligt** GitHub-repo enligt [publiceringsguiden](docs/github-release.md). CI skapar en Release när du pushar en `vX.Y.Z`-tagg.
2. Ange `ägare/reponamn` under **Firmware** i adminvyn och spara.
3. Klicka **Sök uppdatering**. Ladda om sidan för resultatet.
4. Klicka **Installera senaste versionen** när en ny version finns. ESP32 hämtar `manifest.txt` och `YaleBleHomeKit.bin` via verifierad HTTPS, kontrollerar storlek och SHA‑256, skriver den andra app-partitionen och startar om. Låsinställningar och HomeKit-parning ligger kvar i NVS.

Installationen kan ta någon minut. Ha stabil ström och Wi‑Fi. Release-taggen måste matcha versionsnumret i koden.

## Säkerhet och begränsningar

- Det öppna installationsnätet visas när Wi‑Fi saknas. Det ger inte åtkomst till låsnyckeln eller låsreglagen.
- Adminwebben använder HTTP på det lokala nätet. Använd ett betrott nät. Offline key lagras i ESP32 NVS. GitHub Releases är offentliga, men **nycklar och lösenord får aldrig läggas i repot eller i en Release**.
- Koden stöder en BLE-modul som använder `yalexs-ble`-protokollets FE24-tjänst. Alla Yale-modeller har inte verifierats. Firmware har byggts för ESP32 Dev Module med Arduino-ESP32 3.3.12, HomeSpan 2.1.8 och NimBLE-Arduino 2.5.2.
- HomeKit-koden sparas lokalt och visas i adminvyn. Ändrad kod efter parning kan kräva omparning.

## Protokoll och licens

Koden bygger på `yalexs-ble` för [UUID:er och statusvärden](https://github.com/Yale-Libs/yalexs-ble/blob/main/src/yalexs_ble/const.py), [handskakning](https://github.com/Yale-Libs/yalexs-ble/blob/main/src/yalexs_ble/lock.py), [ramar/kryptering](https://github.com/Yale-Libs/yalexs-ble/blob/main/src/yalexs_ble/session.py) och [secure session](https://github.com/Yale-Libs/yalexs-ble/blob/main/src/yalexs_ble/secure_session.py). Se [LICENSE](LICENSE) och [NOTICE](NOTICE).
