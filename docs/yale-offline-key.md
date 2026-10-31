# Hämta offline key och slot från Yale Home

ESP32 behöver två uppgifter som Yale Home använder för lokal Bluetooth-åtkomst till **ditt eget lås**:

- **Offline key:** 16 byte, anges som 32 hextecken utan mellanslag.
- **Slot / key index:** ett heltal 0–255 som hör till samma nyckel och lås.

**Det går inte att se nyckeln eller slot-numret direkt i Yale Home-appen.** Bluetooth-parning i telefonens systeminställningar ger dem inte heller. ESP32 är tänkt att ersätta Yale Connect Bridge: den styr låset lokalt via BLE och behöver inte Yales brygga för själva låskommandona.

| Mål | Enklaste vägen |
| --- | --- |
| Lås i Apple Hem via Home Assistant | [Yale Access Bluetooth](https://www.home-assistant.io/integrations/yalexs_ble/) kan styra lokalt via BLE och dess [Yale Home-integration](https://www.home-assistant.io/integrations/yale) kan ibland hämta offline-nyckeln. En ESP32 kan då vara Bluetooth-proxy. Detta kräver en Home Assistant-server och ger inte automatiskt nyckeln till vår fristående firmware. Yale Home-molnintegrationen anger att många modeller behöver en Connect Bridge eller dörrklocka för molnstyrning. |
| Just denna fristående ESP32-firmware | Du måste ange rå **key** och **slot** i adminvyn. Yale Home har ingen enkel exportknapp. Se alternativen nedan. |

Home Assistants automatiska nyckelhämtning ger inte en nyckel som kan kopieras från dess vanliga gränssnitt till denna firmware. För att göra **vår ESP32** lika enkel att konfigurera behövs en separat, godkänd Yale Home-inloggning som hämtar `OfflineKeys` från kontot och sparar rätt key/slot i ESP32. Det kräver OAuth-klientuppgifter från Yale och kan därför inte lösas genom att lägga ett Yale-lösenord eller ett delat klienthemlighet i öppen firmware. Se [Yales OAuth-dokumentation](https://userguide-developer.aaecosystem.com/oauth/oauth-dance-steps-start-here.html).

## iPhone: via lokal säkerhetskopia (främst Yale Access/August)

1. Öppna **Yale Home** på din iPhone. Kontrollera att låset visas och att ditt konto kan styra det. Ett separat ägarkonto för ESP32 är lämpligt, eftersom appanvändning kan rotera offline-nyckeln.
2. Öppna låsets inställningar i appen och aktivera **Auto-Unlock** under **Automation**. Använd Auto-Unlock minst en gång vid låset. Enligt Home Assistants guide sparas nyckeln då lokalt i appen. [Yales guide till Auto-Unlock](https://www.yalehome.com/au/en/documents/technical-downloads/smart-locks/yale-home/yale-home-app-guide.pdf) visar funktionen.
3. Gör en **lokal** säkerhetskopia av iPhone på din Mac eller PC. Använd ett verktyg som kan läsa appfiler ur säkerhetskopian, exempelvis iMazing eller iPhone Backup Extractor. Home Assistants dokumenterade metod gäller **Yale Access/August-appen**: leta i appens `Library/Preferences` efter en `.plist` som innehåller `key` och `slot` för ditt lås. Öppna binär `.plist` i en plist-läsare (t.ex. Xcode); en vanlig textredigerare visar inte alltid innehållet. För den nyare **Yale Home-appen på iPhone är detta inte garanterat**.
4. Kopiera **key och slot från samma låspost**. Om nyckeln visas som bytes eller base64, konvertera de 16 byten till 32 hextecken. Skriv inte ut dem i terminalhistorik, skärmbilder, supportärenden eller GitHub.
5. Logga in på ESP32:s adminsida, välj låset under **Sök BLE-enheter**, klistra in nyckeln i **Offline key**, ange slot i **Key index / slot** och spara. Kontrollera att status blir **Ansluten** och att låsstatus visas.

Om nyckeln saknas i säkerhetskopian: kontrollera att Auto-Unlock verkligen utlöstes minst en gång och gör en ny säkerhetskopia. Om du använder Yale Home på iPhone och ingen key/slot finns i appens filer, utgå inte från att iPhone-metoden fungerar för din appversion.

## Android: kräver root

Android-metoden kräver rootåtkomst till **din egen telefon**. I Yale Home finns databasen enligt Home Assistant på:

`/data/data/com.assaabloy.yale/databases/ModelDatabase.sql`

Öppna databasen med en SQLite-läsare och hitta tabellen `LockData`, kolumnen `offlineKeys`. Välj posten för rätt lås och läs `key` och `slot` ur JSON-värdet. För äldre Yale Access/August-appen kan sökvägen i stället vara `/data/data/com.august.bennu/databases/ModelDatabase.db` med samma tabell och kolumn. Gör en kopia av databasen för läsning; redigera inte appens originalfil.

Skriv in värdena i ESP32:s adminsida på samma sätt som i iPhone-steg 5.

## Om uppgifterna slutar fungera

Nyckeln kan rotera när samma Yale-konto används i appen. Hämta då en aktuell key/slot från appen och spara den i ESP32 igen. [Home Assistant rekommenderar ett separat ägarkonto](https://www.home-assistant.io/integrations/yalexs_ble/) för stabilare lokal åtkomst. Kontrollera också att modulen annonserar FE24 och att ESP32 är inom Bluetooth-räckvidd.
