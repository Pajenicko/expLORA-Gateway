# expLORA protokol 2. generace – návrh k diskusi

Stav: návrh (17. 10. 2026), nic z toho není implementováno. Cíl: senzory
lorasen v3 (STM32WLE5 v modulu E77) s obousměrnou komunikací, při zachování
podpory 1. generace (MSP430 + RFM95W, XOR "šifra", jednosměrný provoz) na
stejné gateway se stejným jediným rádiem.

## 1. Východiska a omezení

- **Gateway má jedno rádio** (SX1276 na Lite, SX1262 na Pro). Naslouchá na
  jednom kanálu s jedním SF; při vysílání neslyší. Gen1 senzory vysílají na
  868,0 MHz, SF9, BW 125 kHz, CR 4/5, sync 0x12 – **gen2 uplink musí zůstat na
  stejném kanálu a SF**, jinak gateway neuslyší obě generace zároveň.
- Příjem na gateway dnes běží pollingem v `loop()`, který na Pro umí zastavit
  LTE/TLS na sekundy. Pro odpovědi senzoru v pevném časovém okně to nestačí →
  rádio dostane vlastní FreeRTOS task.
- Senzor spí ve Stop2 (~3 µA), 2× AA bez regulátoru. Přijímat smí jen krátce a
  jen tehdy, kdy to sám načasuje (okno po vlastním vysílání). Síťově napájené
  desky (`power-ac`, RS485 s VIN) mohou poslouchat trvale.
- STM32WLE5: 256 kB flash, 64 kB RAM, hardwarové AES (CCM/GCM/CTR), PKA pro
  ECDH, RTC s LSE. Testovací firmware (Arduino + RadioLib) má dnes 75 kB.
- ETSI pásmo: 868,0–868,6 MHz 1 % duty cycle, 25 mW; **869,4–869,65 MHz 10 %
  duty cycle, 500 mW ERP** – přesně pro to má LoRaWAN RX2 na 869,525 MHz.
  Poznámka: nosná 868,0 MHz s BW 125 kHz zasahuje polovinou do 863–868 MHz
  (0,1 % duty cycle) – pro senzory vysílající jednou za 10 min to nevadí,
  pro downlink z gateway by to limit byl.
- Majitel nechce podepisování OTA; gateway je důvěryhodný bod. Autentizace
  obsahu per-device klíčem postačí.

### Proč ne LoRaWAN
LoRaWAN by dal přesně tyto funkce (OTAA, třídy A/C, MAC příkazy, FUOTA), ale
vyžaduje vícekanálovou gateway (8× SX1301/SX1302) a network server
(ChirpStack) – nic z toho na ESP32 s jedním SX1262 realisticky neběží a
jednokanálové "LoRaWAN gateway" jsou v praxi rozbité. Návrh proto LoRaWAN
**napodobuje tam, kde má ověřené řešení** (join s nonce, session key, čítače
rámců, RX okno po uplinku, RX2 kanál, třída A/C), ale zůstává jednoduchý a
point-to-point.

## 2. Přehled

| | Gen1 (dnes) | Gen2 (návrh) |
|---|---|---|
| Směr | jen uplink | uplink + downlink |
| Kanál uplink | 868,0 MHz SF9 | stejný |
| Kanál downlink | – | 869,525 MHz SF9, 10 % DC |
| Identita | SN 24 bit, klíč 32 bit | DevID 32 bit, klíč 128 bit |
| Kryptografie | XOR + XOR checksum | AES-128-CCM, MIC 32 bit, session key, čítače |
| Obsah | pevný formát podle typu zařízení | TLV měření (typ zařízení jen informativní) |
| Příjem na senzoru | – | třída A: okno 1 s po uplinku; třída C: trvale (síťové napájení) |
| Potvrzování | – | per rámec volitelné; politika nastavitelná z gateway |
| Konfigurace | – | interval, TX výkon, ACK politika, čas, slot; verzovaná |
| Akce | – | relé, výstupy, dotaz na stav, reboot |
| OTA senzoru | – | přes gateway, bloky s bitmapou, bootloader v senzoru |

## 3. Rádiové parametry

- **Uplink (senzor → gateway):** 868,0 MHz, SF9, BW 125, CR 4/5, sync 0x12,
  preambule 16, CRC. Shodné s gen1 (jediná možnost při jednom rádiu).
- **Downlink (gateway → senzor):** 869,525 MHz, stejné SF/BW/CR/sync,
  preambule 16 (zvážit 24 pro větší časovou toleranci). Gateway hlídá vlastní
  vysílací čas: klouzavý součet za hodinu ≤ 360 s; při vyčerpání downlink
  odloží a zobrazí varování.
- Výkon: gateway Pro až 22 dBm, Lite 20 dBm (limit 27 dBm ERP). Senzor
  výchozí 14 dBm, nastavitelné 0–22 dBm.
- **RX okno třídy A:** senzor otevře příjem **1000 ms po konci vlastního
  vysílání** (měřeno od TX-done přerušení), o ~20 ms dřív; čeká na preambuli
  max ~150 ms (timeout v symbolech – SX126x umí timeout zrušit po detekci
  preambule), pak buď přijme celý rámec, nebo usne. Spotřeba okna ≈ 5 mA ×
  0,15 s, zanedbatelné proti TX.
- Gateway odpoví **přesně v tomto okně**: časová značka z RX-done přerušení,
  odpověď připravená během < 5 ms v rádiovém tasku, odvysílána časovačem.
  Jitter zdrojů (ISR, SPI, přeladění) je řádově ms, preambule 66 ms dává
  rezervu.
- Krystal E77 bez TCXO: ±10 ppm = ±9 kHz, LoRa při BW 125 toleruje ±31 kHz.

## 4. Identita, klíče, relace

- **DevID**: 32 bit, odvozen z 96bit UID STM32 (jako dnes), na štítku
  **8 hex znaků** (`3EA4191D`). (Gen1 SN zůstává 24 bit dekadicky; v UI
  gateway jsou to dva typy.)
- **Klíč na štítku**: 64 bit náhodných = **16 hex znaků** ve čtveřicích
  (`A1B2-C3D4-E5F6-0718`), pomlčky gateway ignoruje. Zapsán při výrobě do
  flash (poslední stránka). Kratší nejde: `HELLO` dává útočníkovi ověřovací
  orákulum (MIC), 2⁶⁴ AES operací je mimo dosah i GPU farmy, 2⁴⁸ by padlo za
  hodiny. **Root key K (128 bit) = AES-CMAC(štítek ‖ 0×8, DevID)** – obě
  strany ho odvodí, formáty rámců zůstávají 128bitové.
- **Join (jako LoRaWAN OTAA, ale bez serveru):**
  1. Senzor po startu pošle `HELLO` (DevID, devNonce 32 bit náhodný, typ HW,
     verze FW, schopnosti, verze konfigurace); MIC klíčem K, bez šifrování.
  2. Gateway, pokud DevID zná, odpoví v RX okně `HELLO_ACK` (gwNonce, čas,
     aktuální konfigurace), MIC klíčem K, obsah šifrovaný K.
  3. Obě strany odvodí **session key Ks = AES-CMAC_K(0x01 ‖ DevID ‖ devNonce
     ‖ gwNonce)** a vynulují čítače. Dál se K nepoužívá.
  4. Bez odpovědi senzor opakuje `HELLO` s exponenciálním rozestupem
     (1, 2, 4 … max 15 min; stisk tlačítka/reset vynuluje). Gateway neznámé
     DevID ukazuje v UI jako "čeká na přidání" (jako dnes neznámé gen1
     senzory), s typem HW a verzí FW ze zprávy.
- Výsledek: přehrání starých rámců (replay) je nemožné napříč relacemi,
  čítače smějí po restartu senzoru začít od nuly, gateway si nemusí pamatovat
  čítače přes vlastní restart do detailu (po restartu přijme vyšší čítač v okně
  16384; čítače ukládá do `sensors.json` líně, např. 1× za hodinu).
- **Jeden senzor patří jedné gateway** (ta, která má jeho K). Dvě gateway se
  stejným klíčem by si v RX okně odpovídaly přes sebe.
- Volitelně později: **párování bez štítku** – tlačítko na senzoru + "okno
  párování" na gateway, ECDH (X25519/P-256 přes PKA na STM32WL, mbedtls na
  ESP32) → K. Chrání proti odposlechu, ne proti aktivnímu útočníkovi v okně
  30 s; pro domácí nasazení přijatelné, pro výrobu stejně zůstane štítek.

## 5. Rámec

Společná hlavička (nešifrovaná, kryta MIC):

| Byte | Obsah |
|---|---|
| 0 | `0b101 tttt0`: horní 3 bity 101 = gen2, `tttt` typ rámce |
| 1–4 | DevID (big endian) |
| 5–6 | FCnt (dolních 16 bit 32bit čítače; směr up/down má vlastní) |
| 7 | flagy: bit7 ACK požadován, bit6 "mám další data / čekám další downlink", bit5 třída C, bity 0–3 **verze konfigurace** senzoru (mod 16) |
| 8…n−5 | payload, AES-128-CCM(Ks), nonce = DevID ‖ FCnt(32) ‖ směr ‖ 0… |
| n−4…n−1 | MIC 32 bit (CCM tag) |

Režie 12 B. Rámec SHT40 (teplota, vlhkost, baterie v TLV = 9 B) má 21 B ≈
240 ms ve vzduchu při SF9 (gen1: 13 B ≈ 196 ms).

Typy rámců: `HELLO`, `HELLO_ACK`, `DATA` (nepotvrzovaná), `DATA_CONF`
(potvrzovaná), `POLL` (prázdný uplink, jen aby senzor dostal čekající
downlink), `ACK` (downlink, nese i příkazy), `STATUS` (uplink odpověď na
dotaz), `OTA_CHUNK` (down), `OTA_STATUS` (up).

**Rozlišení generací na gateway:** nejdřív zkusit gen2 (hlavička 101…, známé
DevID, MIC sedí – falešná shoda 2⁻³²), až pak dnešní cestu gen1 (zkoušení
klíčů + checksum + SN). Gen1 kód se nemění.

### Payload uplinku: TLV měření

Místo pevných formátů podle typu zařízení nese rámec seznam
`typ(1) hodnota(n)`; délka plyne z typu. Jeden firmware core + daughter tak
posílá jen to, co má, a gateway nepotřebuje parser na každý typ desky.

| Typ | Hodnota | Jednotka |
|---|---|---|
| 0x01 | i16 | teplota ×100 °C |
| 0x02 | u16 | RH ×100 % |
| 0x03 | u16 | tlak ×10 hPa |
| 0x04 | u16 | CO₂ ppm |
| 0x05 | u16 | osvětlení (lux, log kódování) |
| 0x06 | u16 | srážky, čítač překlopení |
| 0x07 | u16 | vítr ×10 m/s (+ 0x08 směr) |
| 0x0A | u16 | baterie mV |
| 0x0B | i8 | RSSI posledního downlinku (diagnostika linku) |
| 0x10 | u8 + u16[] | digitální vstupy: stavová maska + čítače |
| 0x11 | u8 | stav relé (maska) |
| 0x12 | u8 | událost tlačítka (číslo, krátký/dlouhý) |
| 0x20 | u16 reg + u16 val … | surové Modbus registry (RS485) |
| 0x7F | u8 len + data | specifické pro desku |

Gateway drží měření jako mapu `typ → hodnota` a existující `SensorData`
(pevná pole) se plní z ní; HA discovery vzniká z přítomných typů. Typ HW
(sht40, meteo, relay, …) z `HELLO` slouží jen k zobrazení a k nabídce akcí.

### Payload downlinku (`ACK`, `HELLO_ACK`): TLV příkazů

| Typ | Obsah |
|---|---|
| 0x01 | interval hlášení u16 s + offset slotu u16 s |
| 0x02 | TX výkon i8 dBm |
| 0x03 | ACK politika: potvrzovat každý N-tý rámec (0 = nikdy), max. opakování, základ rozestupu |
| 0x04 | čas: unix u32 (+ zóna není potřeba, senzor pracuje v UTC) |
| 0x05 | RX politika: třída A/C, délka okna |
| 0x10 | akce: kanál, stav/impulz, délka (relé, výstupy) |
| 0x11 | dotaz na stav → senzor odpoví `STATUS` v příštím uplinku |
| 0x12 | reboot / přejít do UART bootloaderu |
| 0x20 | OTA start: velikost, CRC32, verze, velikost bloku |
| 0x21 | OTA blok: index u16 + data (v `OTA_CHUNK`) |
| 0x22 | OTA konec / zrušit |
| 0x7F | specifické pro desku |

**Konfigurace je verzovaná, ne potvrzovaná po příkazech.** Gateway drží
"žádanou konfiguraci vN"; senzor v každé hlavičce hlásí verzi, kterou má.
Když nesedí, gateway v nejbližším ACK pošle celou konfiguraci (jsou to
desítky bajtů). Je to idempotentní, přežije ztracené rámce i restarty na obou
stranách a UI jen ukazuje "nastaveno / čeká na senzor". Jednorázové akce
(relé, dotaz, OTA) jdou frontou a potvrzují se dalším uplinkem (stav relé je
v TLV 0x11).

## 6. Chování senzoru

- **Periodická měření (baterie):** `DATA` nepotvrzovaná; ACK požadován
  každý N-tý rámec (výchozí N = 6, tj. ~1× za hodinu při 10 min), aby senzor
  dostal konfiguraci/čas a poznal ztrátu gateway. Bez opakování – další
  vzorek přijde za chvíli stejně. Nedostane-li ACK 3× po sobě, zkusí znovu
  `HELLO` (gateway mohla být přeflashnutá) a zvýší interval ACK.
- **Události (tlačítka, vstupy, zpětná vazba relé):** `DATA_CONF`, opakování
  do 3× s náhodným rozestupem 2–6 s; LED bliká po odeslání a po potvrzení
  (přesně jak chce `NAVRH.md` u REMOTE).
- **Čekající downlink:** ACK má bit "mám další"; senzor pošle do 2 s `POLL`.
  Tím se vyprázdní fronta (více příkazů, OTA) bez čekání na další interval.
- **Relé a jiné akce na baterii:** latence = interval `POLL`/hlášení.
  Nastavitelné (např. 60 s = ~0,5 mAh/den navíc při SF9, 14 dBm → zkrátí
  výdrž na ~1,5 roku). Síťově napájené desky běží ve třídě C a reagují
  okamžitě.
- **Čas a sloty:** z ACK dostává senzor unixový čas (RTC s LSE drift ~1 s/den,
  korekce 1× za hodinu). Gateway každému periodickému senzoru přidělí
  **offset slotu** (zprvu `hash(DevID) mod interval`, později rovnoměrně), takže
  se senzory na jednom kanálu nesrážejí. Bez času vysílá s náhodným jitterem.
- Změna rádiových parametrů (SF/výkon) se aplikuje s pojistkou: nový
  parametr se použije pro další 2 uplinky, bez ACK se vrátí původní.

## 7. OTA firmware senzoru

- **Flash E77 (256 kB):** bootloader 16 kB @0x08000000 + aplikace 120 kB +
  staging 118 kB + konfigurace 2 kB (poslední stránka: K, DevID, kalibrace,
  čítače). Bootloader je vlastní, malý: při startu zkontroluje příznak
  "nový obraz" + CRC32 stagingu, zkopíruje do slotu aplikace, smaže příznak,
  skočí. Vždy je zachován i vstup do ROM UART bootloaderu (GND–RX při
  resetu) a SWD jako záchrana. Aplikace musí vyjít do 120 kB: dnes 75 kB s
  Arduino core, počítat s ~100 kB po přidání AES, TLV, OTA. (Alternativa
  Zephyr + MCUboot – robustnější, ale jiný toolchain a start od nuly;
  nedoporučuji pro teď.)
- **Průběh:** uživatel nahraje `.bin` na gateway (LittleFS, obraz ≤ 120 kB;
  Pro má 7,9 MB, Lite 1,3 MB), vybere senzor(y). Gateway v příštím ACK pošle
  OTA start; senzor (baterie > 2,4 V, jinak odmítne) přejde dočasně do třídy
  C. Gateway posílá bloky po 200 B back-to-back s ohledem na 10 % duty cycle,
  po každých 16 blocích senzor pošle bitmapu chybějících; gateway doplní.
  60 kB obraz = 300 bloků ≈ 180 s ve vzduchu ≈ 30 min včetně duty cycle při
  SF9 (když je RSSI dobré, OTA relace může použít SF7 → ~8 min). Spotřeba
  senzoru ~5 mA × 30 min = 2,5 mAh, zanedbatelné.
- Po příjmu: CRC32 + hlavička obrazu (magic, typ HW, verze, velikost) →
  příznak → reset → bootloader. Důvěra: celá relace je autentizovaná Ks,
  gateway je trust anchor, podpis obrazu se nedělá (shoda s rozhodnutím pro
  gateway).
- Ostatní senzory během OTA: gateway vysílá ~10 % času, takže ztráta jejich
  uplinků ~10 % – přijatelné, UI o tom informuje.

## 8. Změny na gateway

1. **Rádiový task** (priorita nad `loop()`): vlastní SPI k rádiu, obsluha
   DIO přerušení s časovou značkou, parsování hlavičky, vyhledání relace,
   sestavení a odeslání odpovědi v okně (přeladit na 869,525, TX, zpět na
   868,0 a RX), fronta dekódovaných rámců pro `loop()`. Gen1 rámce jen
   předá. `LoRaRadio` dostane `transmit()`/`transmitAt()`; RadioLib je má pro
   oba čipy.
2. **Model senzoru:** generace, K, relace (Ks, nonce), čítače, žádaná
   konfigurace + verze, poslední hlášený stav (FW, baterie, RSSI downlinku),
   schopnosti (třída C, počet relé/vstupů), fronta akcí (RAM + persist),
   stav OTA.
3. **Web:** přidání gen2 senzoru (DevID + 32 hex / vložení z QR), seznam
   zařízení čekajících na přidání, detail senzoru s nastavením (interval,
   výkon, ACK politika, slot) a stavem doručení, tlačítka akcí (relé),
   nahrání firmwaru senzoru + průběh OTA, rozpočet duty cycle.
4. **MQTT/HA:** entity z TLV typů; `switch` pro relé a `number` pro interval
   přes `<prefix>/<id>/set` → fronta downlinku; stav z TLV 0x11.
5. **Kryptografie:** mbedtls CCM/CMAC v ESP-IDF; na senzoru HW AES přes
   HAL (`HAL_CRYP_MODULE_ENABLED` ve `hal_conf_extra.h`) nebo tiny-AES-c
   (~2 kB). Testovací vektory sdílené v obou repech (native testy).
6. **Sdílená specifikace:** hlavičkové soubory s konstantami TLV/typů rámců
   a testovací vektory v samostatném repu (`expLORA-protocol`), vkládaném
   do obou projektů jako knihovna, ať se formáty nerozjedou.

## 9. Spotřeba senzoru (odhad, 10 min interval, 14 dBm, SF9)

| Položka | za den | za rok |
|---|---|---|
| TX 21 B ≈ 240 ms × 45 mA, 144×/den | 0,43 mAh | 158 mAh |
| RX okno 150 ms × 5 mA, 144×/den | 0,03 mAh | 11 mAh |
| Příjem ACK 1× za hodinu | 0,01 mAh | 3 mAh |
| Spánek 4 µA | 0,10 mAh | 35 mAh |
| **Celkem** | | **~210 mAh → 2×AA (2000 mAh) 5+ let**, při 22 dBm ~3 roky |

## 10. Postup (fáze)

1. **Specifikace + testovací vektory** (formát, CCM, CMAC, derivace Ks) –
   sdílené repo, native testy na obou stranách.
2. **Gateway: rádiový task + TX + gen2 `DATA` bez relace** (Ks = K pro
   fázi 2 jen dočasně) a senzor SHT40 vysílající gen2 → ověřit soužití s
   gen1 (meteostanice) na obou gateway.
3. **Join/relace, ACK s konfigurací, verzovaná konfigurace, čas, UI
   nastavení** – senzor umí interval/výkon/ACK politiku a opakování.
4. **Události a akce:** potvrzované uplinky, relé/vstupy/remote, třída C,
   MQTT/HA přepínače.
5. **OTA:** bootloader E77, nahrání obrazu na gateway, přenos s bitmapou,
   UI průběhu; UART bootloader pro servis.
6. Později: sloty z gateway, SF7 pro OTA relace, párování ECDH, adaptivní
   výkon podle RSSI/SNR.

## 11. Rozhodnuto (17. 10. 2026)

- Štítek: DevID 8 hex + klíč 16 hex, K odvozen CMAC. Párování ECDH později.
- Downlink 869,525 MHz. Verzovaná konfigurace. Senzor Arduino + RadioLib +
  vlastní bootloader. Výchozí hodnoty podle bodu 6.
- Relé a akční členy se zatím neřeší (formát TLV 0x10/0x11 zůstává rezervován).

## Původní otázky

1. Klíč 128 bit na štítku (QR + 32 hex) jako základ – ano? Párování ECDH až
   potom?
2. Downlink na 869,525 MHz (10 %) místo 868,0 MHz (1 %) – souhlas? (Anténa
   T3-868M to zvládne, rozdíl 1,5 MHz.)
3. Verzovaná konfigurace místo potvrzování každého příkazu.
4. Senzor zůstane Arduino + RadioLib + vlastní bootloader (ne Zephyr/MCUboot).
5. DevID 32 bit hex na štítku (gen1 má 6 číslic dekadicky) – nebo zachovat
   dekadické 24bitové SN i pro gen2?
6. Výchozí hodnoty: interval 10 min, 14 dBm, ACK každý 6. rámec, 3 opakování
   událostí, POLL relé 60 s.

## 12. Změny po nezávislé revizi (Opus 5.5, 17. 10. 2026) – tyto body mají přednost před sekcemi 3–10

Revize ověřila RadioLib 7.1.2/7.8.1, Arduino-ESP32 3.3.12 a STM32duino ve
zdrojích. Přijaté změny:

### Časování okna
- Airtime při SF9/125: symbol 4,096 ms, preambule 16 + 4,25 symbolů =
  **83 ms**, hlavička platná ~116 ms po začátku TX; 21 B rámec 218 ms,
  ~40 B `HELLO_ACK` ~320 ms, 215 B OTA blok **1,10 s**.
- RadioLib neposílá `StopTimerOnPreamble` – timeout příjmu běží až do
  detekce hlavičky. **RX1: otevřít v T−20 ms, timeout 200 ms** (pokryje
  zpoždění gateway do ~50 ms + 116 ms hlavička). Cena ~7 mAh/rok.
- **RX2 záložní okno v T+2 s** na stejné frekvenci, jen pokud RX1 nic
  nezachytilo (výpadek gateway, gateway zrovna přijímala jiný rámec).
- Preambule zůstává 16. Zpoždění 1 s zůstává.
- Senzor si časuje od TX-done přerušení; jitter senzoru (~±5 ms, alarm RTC)
  změřit a zahrnout.
- **RadioLib stejná verze v obou projektech, s `stageMode()/launchMode()`**
  (7.8.x; gateway je dnes připnutá na 7.1.2, senzor přes `^7.1.2` už 7.8.1).
  Gateway: v T−10 ms standby (`STANDBY_XOSC`, ne RC – jinak +5 ms na TCXO
  u SX1262), `setFrequency(869.525)`, `stageMode(TX)`, busy-wait na
  `esp_timer_get_time()`, `launchMode()` v T; po TX-done zpět 868,0 a RX.
  Přeladění o 1,5 MHz nevyvolá rekalibraci obrazu (práh 20 MHz).
- Jednorázově změřit pevný offset každého čipu (latence RxDone + rozběh TX)
  a odečíst.
- **Lite (SX1276/RFM95W):** `setCurrentLimit(140)` před TX (výchozí OCP 60 mA
  by ořezal PA_BOOST), max **17 dBm** (+20 dBm režim má limit 1 % duty cycle).
  V tasku rozlišovat TxDone/RxDone podle IRQ flagů (DIO0 se při TX přemapuje).
- Před přepnutím do TX zkontrolovat, že rádio zrovna nepřijímá
  (PreambleDetected/HeaderValid, resp. ModemStat); přednost dát jen join, ACK
  událostí a OTA, jinak nechat na RX2 / další uplink.
- **ISR na ESP32 nejsou v IRAM** (`CONFIG_ARDUINO_ISR_IRAM` není nastaveno):
  mazání sektoru flash (LittleFS, NVS, OTA) zastaví na 30–50 ms i rádiový
  task. Řešení: kooperativní "povolení zápisu do flash" – rádiový task ho
  drží od RxDone do TxDone, když čeká odpověď; `saveSensors()`, ukládání
  konfigurace a nahrávání souborů ho berou před zápisem.
- Logovat `getFrequencyError()` per senzor (TCXO E77 proti krystalu RFM95W
  na Lite v mrazu – rezerva proti ±31 kHz hlídat).
- E77 **má TCXO** (32 MHz, napájení 1,7 V z PB0-VDDTCXO) – ověřeno v M0;
  s `tcxoVoltage = 0` rádio hlásí XOSC_START_ERR. Firmware i README senzoru
  opraveny (původní tvrzení, že pracují s krystalem, bylo omylem).

### Kryptografie a relace
- **Relace se na gateway neukládají.** Ukládá se jen K (resp. štítek) a
  poslední devNonce. Po startu gateway odpoví na první gen2 uplink senzoru
  příkazem `REJOIN` v jeho okně, autentizovaným K a svázaným s DevID, FCnt a
  MIC toho uplinku (nelze přehrát). Tím odpadá opakování nonce po
  nečistém restartu i otázka opotřebení flash.
- **devNonce je monotónní 32bit čítač v senzoru** (zapisuje se jen při
  joinu); gateway drží poslední hodnotu per DevID a nižší/stejnou zahodí
  (přehraný `HELLO` nevynutí rekey). Stará relace se drží vedle nové, dokud
  první uplink pod novým Ks neprojde. Limit: `HELLO_ACK` nejvýš 1× za 10 s
  per DevID a max 20 % rozpočtu duty cycle.
- **Oddělení domén klíčů:** K = CMAC(štítek‖0×8, DevID);
  K_join = CMAC_K(0x02‖DevID); Ks = CMAC_K(0x01‖DevID‖devNonce‖gwNonce).
  Nonce join zpráv = DevID‖devNonce‖směr‖typ rámce. Typ rámce je součástí
  nonce/AAD u všech rámců.
- **Downlink je svázán s potvrzovaným uplinkem:** FCnt32 potvrzeného uplinku
  je v AAD downlinku (jako ConfFCnt v LoRaWAN 1.1).
- **Každé vysílání má nový FCnt** (i opakování). Události nesou vlastní
  pořadové číslo kvůli deduplikaci na gateway. Senzor nikdy neukládá Ks bez
  monotónního FCnt – proto se FCnt neukládá vůbec a po restartu se dělá join.
- **Konstrukce:** jeden AES-128-ECB primitiv na platformu (mbedtls s HW AES
  na ESP32 – `MBEDTLS_CMAC_C`/`CCM_C` jsou zapnuté; HAL CRYP na STM32WL
  v STM32duino standardně **není** – zapíná se build flagy
  `-DHAL_CRYP_MODULE_ENABLED -DHAL_RNG_MODULE_ENABLED` (ověřeno v M0), nebo
  ~2 kB softwarové AES) + **společná
  přenositelná CMAC + CTR** v knihovně `expLORA-protocol` (LoRaWAN styl: CTR
  pro payload, CMAC přes B0‖hlavička‖šifrotext, MIC 32 bit). Jednodušší na
  přenos vektorů než HAL CCM s ručně skládanými B0/B1. Nonce ze STM32WL
  TRNG (HAL_RNG), ne z `radio.randomByte()`.
- Rozlišení generací: pokud byte 0 sedí a DevID je **známé** gen2 zařízení
  bez relace, zpracovat v gen2 cestě (poslat `REJOIN`), nepadat do gen1.
  `HELLO` má pevnou délku a 2bajtový magic; do seznamu "čeká na přidání" až
  po 2 shodných `HELLO`; seznam omezit, vyřazovat nejstarší.
- `sensors.json`: držet poslední dobrou kopii `sensors.bak` a při poškození
  se k ní vrátit; export/import klíčů chráněný admin heslem.
- Dvě gateway se stejným K: dokumentovat; "známé DevID, MIC stále selhává"
  → varování, že senzor je spárovaný jinde. DevID z FNV hashe UID: ~0,3 %
  kolizí při 5000 kusech → kontrola proti výrobní databázi, duplicitu v UI
  odmítnout.

### Formát rámce a TLV
- **Čtyři typy rámců:** `JOIN_REQ`, `JOIN_ACC`, `UP`, `DOWN`. `POLL` = `UP`
  s prázdným payloadem, potvrzovaný rámec = `UP` s ACK flagem, stav/OTA stav
  = TLV v `UP`, ACK/OTA blok = TLV v `DOWN`. Uvolněné bity hlavičky nesou
  **verzi protokolu**; verze protokolu je i v `JOIN_REQ`/`JOIN_ACC`.
- **TLV přeskočitelné:** horní 2 bity typu = třída délky (1 B / 2 B / 4 B /
  bajt délky následuje). Neznámý typ se přeskočí.
- **Index kanálu** u měření (DS18B20 více čidel, SCD40 vedle SHT40, vstupy
  per kanál) – stabilní ID pro HA entity. Neplatná hodnota = sentinel
  (`0x8000` / `0xFFFF`). Čítače impulzů u32. Vítr: průměr + náraz + směr.
  Lux: u16 log kódování definovat. Modbus: slave id + funkce + počáteční
  registr. Nové TLV: zdroj napájení (typ podle VSENSE, nabíjení),
  diagnostika (příčina resetu, SNR downlinku), příkaz desky (SCD40 forced
  recalibration). Událost nese pořadové číslo.
- 4bitová verze konfigurace stačí, protože `JOIN_ACC` vždy nese celou
  konfiguraci a po startu gateway vynutí rejoin. Čas = "unix čas v okamžiku
  začátku TX tohoto downlinku".

### Chování senzoru
- **RX1 se otevírá po každém uplinku** (0,03 mAh/den); ACK flag znamená jen
  "musíš odpovědět". Konfigurace, `REJOIN` a příkazy tak jdou v kterémkoli
  okně; `POLL` slouží jen k vyprázdnění fronty.
- Stavový automat join/rejoin/backoff je součástí specifikace: po 3
  chybějících ACK senzor **dál posílá data pod starou relací** a souběžně
  zkouší `JOIN_REQ` s exponenciálním rozestupem (1 min … 15 min); po úspěšném
  joinu přejde na novou relaci.
- Pomalá měření (SCD40 ~5 s, DS18B20 750 ms) začít dřív, aby rámec odešel ve
  slotu.

### Regulace
- **Na 868,0 MHz je limit 25 mW ERP = 14 dBm ERP (~16 dBm EIRP).** Výkon
  senzoru se omezí ve firmware podle zisku antény; 22 dBm je přípustných jen
  v 869,4–869,65 MHz. Odhad výdrže "22 dBm" v §9 a v NAVRH neplatí.
- Nosná 868,0 MHz zasahuje do 863–868 MHz → konzervativně **0,1 % duty cycle
  (3,6 s/h) na senzoru**: 10min data vyhoví (1,3 s/h), `POLL` každých 60 s
  (~12 s/h) a OTA bitmapy ne. **Senzor si vede účetnictví duty cycle.**
  Uplink frekvence je konfigurační parametr (se stejnou pojistkou návratu
  jako SF/výkon), aby se instalace bez gen1 mohly přesunout na 868,1/868,3.
- 869,525 MHz je zároveň RX2 a beacon veřejných LoRaWAN sítí (CRA, TTN).
  **Normální IQ** (ne invertované) + sync 0x12 dávají dva rozlišovací znaky
  proti LoRaWAN downlinkům (invertované IQ, sync 0x34). RX2 záloha pokryje
  občasné rušení. Downlinky gateway **rozestupovat** (off-time ≥ 9× airtime),
  nikdy ne dávky, které gateway na minuty ohluší; OTA plánovat v noci.

### OTA a bootloader (rozhodnutí pro layout, přenos až později)
- **Layout flash musí být finální před expedicí prvních senzorů**, jinak je
  nelze nikdy aktualizovat vzduchem. Proto bootloader + layout patří do první
  fáze; přenos po LoRa až později.
- **Swap s potvrzením místo kopie:** dva stejné sloty (schválený layout: 2 × 112 kB, viz `docs/protocol-v2.md` §10) +
  2 kB scratch + stránka stavu swapu. Nová aplikace se **potvrdí až po
  úspěšném joinu**; jinak po N startech pod IWDG bootloader přehodí zpět.
  Bootloader bare-metal (CMSIS/LL, 4–8 kB), ne Arduino. Při skoku vypnout
  SysTick a NVIC linky, vymazat pending, nastavit MSP, PRIMASK vymazaný
  (STM32duino `__enable_irq()` nevolá).
- **Identita (DevID, štítek, revize HW) na vlastní stránce flash, která se po
  výrobě nikdy nemaže**, odděleně od konfigurace a devNonce (log-structured
  stránka). OTP (1 kB, write-once) zvážit pro výrobu; u vývojových kusů
  ne (nešlo by změnit klíč). FCnt se neukládá.
- STM32WL flash: zápis 64bit s ECC, každé dvojslovo jen jednou po mazání →
  před zápisem bloku zkontrolovat bitmapu (duplicitní blok = PROGERR/ECC).
  Bloky 200 B (zarovnané na 8). Příznaky jako append-only záznamy. BOR_LEV
  ~2,0–2,2 V v option bytes; žádné operace s flash během TX; práh 2,4 V pro
  OTA měřit pod zátěží. IWDG: option bit IWDG_STOP (jinak 32 s max ve
  Stop2). ROM UART bootloader: podle AN2606 ověřit USART/piny pro WLE5.
- Aplikace za bootloaderem (schváleno: 0x08002000) v STM32duino funguje (`board_build.flash_offset` →
  `VECT_TAB_OFFSET` + `LD_FLASH_OFFSET`, VTOR se nastavuje). Nutné:
  `board_upload.maximum_size` = offset + slot (jinak aplikace tiše přeroste),
  `upload_command` nesmí programovat na 0x08000000, post-build trailer
  (magic, typ HW, verze, velikost, CRC).
- Čas OTA: 215 B blok 1,1 s → 60 kB ≈ 55 min, reálných 100–120 kB ≈ 100–110
  min při SF9 a 10 %; SF7 ~3× rychlejší.

### Architektura gateway
- Do rádiového tasku se přesouvá **vše**, co dnes volá RadioLib: 10min
  `isConnected()/reset()`, `getRSSI()/getSNR()`, flag přerušení. Zprávy ve
  frontě nesou RSSI, SNR, chybu frekvence a časovou značku. `SPIManager` je
  privátní sběrnice tasku. Task vzniká staticky při startu
  (`xTaskCreateStaticPinnedToCore`, core 1, priorita 12–15: nad AsyncTCP 10,
  pod esp_timer 22). Nikdy blokující `transmit()` (točí se celý airtime);
  start + notifikace z DIO ISR.
- Rádiový task **nikdy nebere `sensorMutex` a nesahá na LittleFS**; má
  vlastní malou tabulku relací (DevID, K, Ks, čítače, čekající downlink),
  komunikace frontami. OTA bloky mu `loop()` předčítá do kruhového bufferu.
- Statická paměť: zásobník ~6 kB, fronta 8 rámců ~2,5 kB, tabulka ~2 kB,
  OTA buffer ~2 kB – vše při startu, aby to hlídač fragmentace neviděl.
- Už samotný přesun příjmu do tasku zlepší gen1 (dnes se buffer přepisuje,
  když `loop()` stojí na LTE) → nasadit jako samostatný krok s regresí gen1.

### Dopad na gen1 (odhad)
Běžný provoz ≈ 0,3 % ztrát navíc (downlinky), gen2 provoz samotný ≈ +1,5 %
(ALOHA kolize), během OTA ≈ 12 % po dobu 1–2 h; dávkové OTA by dalo až 60 %
(proto rozestupy). Přesun RX do tasku na Pro pravděpodobně všechno převáží.

### Fáze (opravené pořadí)
0. **Spike:** nešifrovaný ping/pong na obou gateway s histogramem
   RxDone→TX-start pod zátěží (nahrávání, MQTT TLS, ukládání LittleFS, LTE)
   a úspěšností okna na senzoru; na E77 bootloader + aplikace na
   0x08004000, zápis do druhého slotu, Stop2, IWDG, změřená velikost s
   RadioLib a kryptografií.
1. Specifikace + testovací vektory včetně stavových automatů, tabulky
   časových konstant, pravidel duty cycle a layoutu flash.
2. 2a: rádiový task jen s gen1 + regrese; 2b: gen2 `UP`.
3. Join a relace rovnou (žádná zkratka Ks = K).
4. Události, akce, třída C. 5. Přenos OTA.

### Chybí a doplní se později
Výroba a provisioning (generování klíčů, zápis identity, tisk štítku/QR,
databáze DevID s kontrolou duplicit), účetnictví duty cycle na senzoru,
export/záloha klíčů na gateway, simulátor senzoru pro testy gateway,
statistika úspěšnosti downlinků per senzor, "zapomenout gateway"/tovární
reset senzoru, autentizace MQTT `/set` příkazů (relé), stabilita ID entit HA.
