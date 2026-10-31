# Publicera projektet och firmware på GitHub

Repo och Releases behöver vara **offentliga**, eftersom ESP32 laddar ner dem utan GitHub-inloggning. Repo innehåller endast källkod. Offline key, Wi‑Fi-lösenord, adminlösenord och HomeKit-kod finns lokalt på ESP32 och ska aldrig publiceras.

## Skapa repo och skicka upp koden

1. Logga in på GitHub och skapa ett tomt **public** repo, exempelvis `Yale-ble-esp`. Lägg inte till en README eller licens i GitHub-formuläret; dessa finns redan här.
2. I projektets katalog, kör (byt `DIN_ANVÄNDARE` och reponamn):

   ```sh
   git remote add origin https://github.com/DIN_ANVÄNDARE/Yale-ble-esp.git
   git push -u origin main
   ```

   Om GitHub CLI är inloggat kan repot i stället skapas och pushas med `gh repo create Yale-ble-esp --public --source=. --remote=origin --push`.

## Skapa första uppdateringsbara Release

1. Kontrollera att `YALE_FW_VERSION` i `.ino` är `0.2.0`.
2. Skapa och skicka taggen:

   ```sh
   git tag v0.2.0
   git push origin v0.2.0
   ```

3. GitHub Actions bygger firmware med `no_fs` och skapar Release med `YaleBleHomeKit.bin` samt `manifest.txt`. Vänta tills arbetsflödet **Firmware release** är grönt under **Actions**.
4. Skriv `DIN_ANVÄNDARE/Yale-ble-esp` i ESP32:s adminvy under **Firmware**. Tryck **Sök uppdatering**, ladda om sidan och installera om en nyare version visas.

För nästa version: ändra `YALE_FW_VERSION`, committa och pusha ändringen, skapa sedan motsvarande `vX.Y.Z`-tagg. Skicka aldrig en tagg innan kodändringen har pushats. Om arbetsflödet saknar rätt att skapa Release, kontrollera repots **Settings → Actions → General → Workflow permissions** och tillåt läs- och skrivrättigheter för workflows.

Första USB-installationen måste använda `no_fs` med två app-partitioner. En gammal installation med `Huge APP` kan inte göra den första OTA-uppdateringen.
