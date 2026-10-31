# Hämta offline key och slot från Yale Home

ESP32 behöver två uppgifter som Yale Home använder för lokal Bluetooth-åtkomst till **ditt eget lås**:

- **Offline key:** 16 byte, anges som 32 hextecken utan mellanslag.
- **Slot / key index:** ett heltal 0–255 som hör till samma nyckel och lås.

Yale Home visar normalt inte dessa värden i appens gränssnitt. Bluetooth-parning i telefonens systeminställningar ger dem inte heller. Metoderna nedan bygger på [Home Assistants dokumentation för Yale Access Bluetooth](https://www.home-assistant.io/integrations/yalexs_ble/). Appversioner och lagringsformat kan ändras.

## iPhone: via lokal säkerhetskopia

1. Öppna **Yale Home** på din iPhone. Kontrollera att låset visas och att ditt konto kan styra det. Ett separat ägarkonto för ESP32 är lämpligt, eftersom appanvändning kan rotera offline-nyckeln.
2. Öppna låsets inställningar i appen och aktivera **Auto-Unlock** under **Automation**. Använd Auto-Unlock minst en gång vid låset. Enligt Home Assistants guide sparas nyckeln då lokalt i appen. [Yales guide till Auto-Unlock](https://www.yalehome.com/au/en/documents/technical-downloads/smart-locks/yale-home/yale-home-app-guide.pdf) visar funktionen.
3. Gör en **lokal** säkerhetskopia av iPhone på din Mac eller PC. Använd ett verktyg som kan läsa appfiler ur säkerhetskopian, exempelvis iMazing eller iPhone Backup Extractor. Välj Yale Home/Yale Access-appens filer och leta i `Library/Preferences` efter en `.plist` som innehåller `key` och `slot` för ditt lås. Öppna binär `.plist` i en plist-läsare (t.ex. Xcode); en vanlig textredigerare visar inte alltid innehållet.
4. Kopiera **key och slot från samma låspost**. Om nyckeln visas som bytes eller base64, konvertera de 16 byten till 32 hextecken. Skriv inte ut dem i terminalhistorik, skärmbilder, supportärenden eller GitHub.
5. Logga in på ESP32:s adminsida, välj låset under **Sök BLE-enheter**, klistra in nyckeln i **Offline key**, ange slot i **Key index / slot** och spara. Kontrollera att status blir **Ansluten** och att låsstatus visas.

Om nyckeln saknas i säkerhetskopian: kontrollera att Auto-Unlock verkligen utlöstes minst en gång, gör en ny säkerhetskopia och prova igen. Om ditt Yale Home-konto kan anslutas till Home Assistant kan dess Yale-integration i vissa fall hämta offline-uppgifter automatiskt; se den länkade Home Assistant-guiden.

## Android: kräver root

Android-metoden kräver rootåtkomst till **din egen telefon**. I Yale Home finns databasen enligt Home Assistant på:

`/data/data/com.assaabloy.yale/databases/ModelDatabase.sql`

Öppna databasen med en SQLite-läsare och hitta tabellen `LockData`, kolumnen `offlineKeys`. Välj posten för rätt lås och läs `key` och `slot` ur JSON-värdet. För äldre Yale Access/August-appen kan sökvägen i stället vara `/data/data/com.august.bennu/databases/ModelDatabase.db` med samma tabell och kolumn. Gör en kopia av databasen för läsning; redigera inte appens originalfil.

Skriv in värdena i ESP32:s adminsida på samma sätt som i iPhone-steg 5.

## Om uppgifterna slutar fungera

Nyckeln kan rotera när samma Yale-konto används i appen. Hämta då en aktuell key/slot från appen och spara den i ESP32 igen. [Home Assistant rekommenderar ett separat ägarkonto](https://www.home-assistant.io/integrations/yalexs_ble/) för stabilare lokal åtkomst. Kontrollera också att modulen annonserar FE24 och att ESP32 är inom Bluetooth-räckvidd.
