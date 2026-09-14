# Firmware and security

State of public knowledge as of 2026-09-14. Evidence tags: see [README](../README.md#evidence-tags).

## TL;DR

- No public writeup covers the Canvas controller's OS, bootloader or firmware internals. Sibling-product evidence points to **OpenWrt/Linux on MIPS**: Aurora runs OpenWrt on RT5350; the CVE-2022-47758 writeup shows OpenWrt 19.07.4 on `ramips/mt76x8` — the same OpenWrt target as Canvas's MT7688AN.
- Canvas shares the panel-product firmware line (Shapes/Elements/Lines) since 9.2.0. Latest Canvas release: **12.4.1 (2026-06-02)**. Support status: "Support Intermittent".
- **Canvas never received Thread or Matter**, despite a CES 2023 announcement. Its EFR32BG1B radio is BLE-only, so Thread is not possible on this hardware ([hardware.md](hardware.md)).
- Firmware images are hosted on public S3 over plain HTTP (`canvas-firmware.s3.amazonaws.com/<version>.firmware`). Nanoleaf has published nothing about signing, encryption or secure boot.
- Five CVEs mention Nanoleaf; none names Canvas, but Canvas received the "security fix" releases that coincide with CVE-2022-47758 and CVE-2026-33268.
- Nanoleaf has released no GPL source.

## 1. Firmware version history (Canvas)

Source: Nanoleaf release notes ([2026](https://support.nanoleaf.me/hc/en-us/articles/45269445987092-Products-Firmware-Release-Notes-2026), [2025](https://support.nanoleaf.me/hc/en-us/articles/35633948389268-Products-Firmware-Release-Notes-2025), [2024 archive](https://support.nanoleaf.me/hc/en-us/articles/33006784349076--2024-Archive-9-4-0-Firmware-Release-Notes-Panel-Products), [2023 archive](https://support.nanoleaf.me/hc/en-us/articles/32800486435348--2023-Archive-9-3-4-or-Older-Firmware-Release-Notes-Panel-Products)). Only entries listing Canvas. Nanoleaf "combined all releases older than 2023", so early history may be incomplete. [A]

| Version | Date | Canvas-relevant note |
|---|---|---|
| 1.1.0 | 2018-12-20 | "Added events to Open API" |
| 1.2.0 | 2019-03-12 | Brightness sensor; "OpenAPI improvements… custom animData" |
| 1.4.0 | 2019-07-22 | Light Squares as HomeKit buttons; cloud backup/restore; "Touch Data Stream to Canvas' Open API" |
| 1.5.0 | 2019-09-11 | "Support for control over the Nanoleaf Cloud" |
| 1.6.0 | 2019-11-28 | Playlists; pairing-code pairing in Desktop App |
| 1.6.1 / 1.6.2 / 1.6.4 | 2019-12 – 2020-08 | Crash-on-restore fix; Wi-Fi and cloud connectivity |
| 5.2.4 | 2021-04-20 | Panel–controller communication |
| 6.2.1 | 2021-10-08 | "Control Squares to become Cloud Gateways for Nanoleaf Essentials" |
| 6.5.1 | 2022-06-17 | Cloud connection fix |
| **7.1.3** | 2023-02-07 | "**Security vulnerability fixed.**" (Elements, Shapes, Canvas) |
| 7.1.6 | 2023-05-25 | Wi-Fi provisioning fix |
| 9.2.0 / 9.2.2 / 9.2.4 | 2023-08/09 | Sync+; Canvas instability fix |
| 9.3.3 | 2023-12-13 | "critical issue with Nanoleaf Cloud" |
| 9.6.4, 11.2.1 | 2024-07-26, 2024-09-17 | Bug fixes |
| 12.0.3, 12.2.0, 12.3.2 | 2025-02-06, 06-17, 09-10 | Bug fixes; Routines and circadian schedules |
| **12.3.6** | 2026-03-24 | "**Security and performance enhancements**" |
| 12.4.1 | 2026-06-02 | "Security and performance enhancements" — latest for Canvas |

Numbering: no official explanation. Since 9.2.0 Canvas shares version numbers with Shapes, Lines and Elements [A]. The jump 1.6.4 → 5.2.4 suggests Canvas was merged into the shared panel line [C].

Support: Nanoleaf's support-period table lists "Before 2020 | Support Intermittent | Canvas (NL29*)"; Shapes/Elements guaranteed to 2026-10-31, Lines to 2027-08-31 [A, [support FAQ](https://support.nanoleaf.me/hc/en-us/articles/34733320939028-FAQ-Warranty-Nanoleaf-Product-Support-Security)].

### Thread / Matter

- The releases that *added* Thread Border Router support (6.1.2, 8.5.2) list only Elements/Shapes(/Lines); Nanoleaf's [Thread FAQ](https://support.nanoleaf.me/hc/en-us/articles/33037480104980-FAQ-Thread-Nanoleaf-Thread-Border-Routers) lists only Elements, Lines, Shapes [A].
- Caveat: the 9.2.3 entry dated 2023-09-18 is headed "ELEMENTS | SHAPES | CANVAS" and says "Improved Thread Border Router performance for Matter and HomeKit Thread devices" [A]. Given the hardware and the 8.5.2 exclusion, this reads as a shared release-note text for the combined firmware line rather than evidence of Thread on Canvas [C]. Some community sources take it literally.
- Nanoleaf's CES 2023 press release said Canvas would be "Matter upgradeable later this year" and listed it as a Thread Border Router ([reprint](https://www.digitalreviews.net/news/pressers/what-matters-is-happening-with-nanoleaf-at-ces-2023/)) [A, press].
- The Canvas BLE chip (EFR32BG1B232) has no 802.15.4 radio, so Thread was never possible on this hardware; Matter-over-Wi-Fi would have been the only path, and it was not delivered [C, backed by datasheet].

## 2. Update delivery, OS and bootloader

| Fact | Tag | Source |
|---|---|---|
| Updates are triggered from the app (More > My Devices > Update); keep the app open near the device | A | [Nanoleaf update guide](https://support.nanoleaf.me/hc/en-us/articles/36891488204564-Smart-Tip-Firmware-How-Where-To-Update-Firmware) |
| In hotspot mode the "Control Square will not be able to download firmware updates" → the controller downloads the image itself | A / C | Canvas user manual |
| No public statement on signing, encryption or secure boot | A (absence) | Release notes, FAQs, ToU, privacy policy |
| Firmware hosted on per-product S3 buckets over **HTTP**, e.g. `http://canvas-firmware.s3.amazonaws.com/<version>.firmware` | B | [dagbdagb notes](https://github.com/dagbdagb/nanoleaf-firmware-upgrade-notes) |
| 63 Canvas images (1.1.0 – 12.4.1) are on the bucket; all downloaded 2026-09-14 to `firmware/canvas/` and verified against the S3 ETags. Versions, sizes, S3 dates and hashes are in `firmware/canvas/manifest.tsv` | A | Probing every version number from the release notes |
| No readable header; byte entropy 7.94–7.99 bits/byte, so the images are compressed or encrypted as a whole | A (measured) / C (interpretation) | Local measurement |
| "(at least Elements)" exposes a local firmware upload page at `http://192.168.2.1/` in setup mode; old devices must be upgraded in multiple steps | B | dagbdagb notes |
| Controllers "appear to run a fork of OpenWRT with linux kernel 4.4.something on a fairly old MIPS microcontroller" (author doesn't own the hardware) | B | dagbdagb notes |
| **Aurora (NL22)**: RT5350, UART boot log at 57600 shows "MIPS OpenWrt Linux-3.18.18"; U-Boot env not usefully modifiable; shell password-protected | B | [OpenWrt forum](https://forum.openwrt.org/t/nanoleaf-light-panels/81748) |
| **Unnamed Nanoleaf product** (CVE-2022-47758 writeup): OpenWrt 19.07.4, `ramips/mt76x8`, Linux 4.14.195, a `cloud_daemon` MQTT client, mbedTLS | A (researcher) | [pwning.tech](https://pwning.tech/cve-2022-47758/) |
| Canvas OS / RTOS / bootloader specifically | Not public | — |

The Canvas firmware image is the most promising public artefact for the panel protocol too: Panton reports the Shapes controller firmware contains symbol names for panel commands ([panel-bus.md](panel-bus.md)).

## 3. Published security research

### CVEs

NVD keyword search "nanoleaf" returns exactly five records ([NVD API](https://services.nvd.nist.gov/rest/json/cves/2.0?keywordSearch=nanoleaf)) [A].

| CVE | Product | Issue | Fixed |
|---|---|---|---|
| CVE-2022-46640 | Nanoleaf Desktop App < 1.3.1 (Windows) | Unauthenticated command injection via `/validateWifiPassword` on `0.0.0.0:56751`; CVSS 9.8 | Dec 2022 ([writeup](https://pwning.tech/cve-2022-46640/)) |
| CVE-2022-47758 | "Nanoleaf firmware v7.1.1 and below" | Missing TLS certificate verification on the cloud connection → RCE via network MITM + MQTT command channel; CVSS 9.8 | "patched… early January 2023" ([writeup](https://pwning.tech/cve-2022-47758/)) |
| CVE-2023-42189 | Matter SDK 1.1.0.0 incl. "Nanoleaf Light strip v3.5.10" | KeySetRemove DoS; CVSS 7.5 | SDK fix ([issue #28518](https://github.com/project-chip/connectedhomeip/issues/28518)); Nanoleaf fix not stated |
| CVE-2023-45955 | Nanoleaf Light strip 3.5.10 | DoS via crafted write-binding commands; CVSS 7.5 | Not stated |
| CVE-2026-33268 | Nanoleaf Lines 12.3.2 | "does not authenticate firmware file uploads" → storage exhaustion; CVSS 3.1 6.5 / 4.0 6.9; CISA advisory VA-26-084-01 | 12.3.6 ([CVE record](https://cveawg.mitre.org/api/cve/CVE-2026-33268)) |

Relevance to Canvas [C]:
- CVE-2022-47758: Canvas's 7.1.3 "Security vulnerability fixed" (2023-02-07) plausibly corresponds (affected range ends at 7.1.1). Not confirmed by Nanoleaf.
- CVE-2026-33268: names only Lines, but 12.3.6 ("Security and performance enhancements") shipped to Canvas the day before publication. Plausibly affected; not confirmed.

### Advisories, talks, papers

- Nanoleaf has no readable advisory page. Its support FAQ points to a Product Security page; search snippets indicate `product-security@nanoleaf.me`, a PGP key, and a 1–2 week response target (page did not render) [B].
- 2019 student MITM project on Light Panels firmware 2.3.0: panels "depend on external service hosted from Amazon" ([elvis.science wiki](https://wiki.elvis.science/index.php?title=Nanoleaf_Light_Panels_-_Hacking)) [A].
- Lazzaro et al., PerCom 2024 ([arXiv 2401.12184](https://arxiv.org/html/2401.12184)): "Nanoleaf triangle" local API vulnerable to replay; cleartext responses [A].
- Andrews et al., 2025 ([arXiv 2501.06033](https://arxiv.org/html/2501.06033)): Shapes seen using TCP, TLS, mDNS, SSDP [A].
- IoTLS (IMC 2021) and Jakaria et al. (PoPETs 2024) do not cover Nanoleaf [A].
- Consumer Rights Wiki claims very high DNS query volume and lists `collector.nanoleaf.com`, `apollo.nanoleaf.com`, `iaso.nanoleaf.com` ([consumerrights.wiki](https://consumerrights.wiki/w/Nanoleaf)); a cited Reddit thread title says `collector.nanoleaf.me` — domain suffix unverified [B].

No conference talk specific to Nanoleaf found.

## 4. Reset, pairing, local auth

From the Canvas user manual ([PDF](https://content.syndigo.com/asset/d8b56bf3-9f78-40d4-a175-168e12b38958/original.pdf), undated) [A]:

| Action | Procedure | Effect |
|---|---|---|
| Soft reset | Hold Power + Dim 15 s | Resets Wi-Fi and pairing; scenes kept |
| Hard reset | Hold Power + Dim while plugging in; release within 1 s of LEDs lighting | Deletes Wi-Fi, pairing, scenes |
| Wi-Fi-only reset | Alternate Power then Dim, 5 times | Wi-Fi settings |
| Hotspot mode | Hold Rhythm + Power 30 s | Local AP; no firmware downloads |
| API/remote pairing window | Hold Power 3 s → 30 s window | Pair Nanoleaf Remote or third-party app |

HomeKit: 8-digit setup code / QR on the Welcome Insert or Control Square, or NFC tap. A lost code requires a support ticket with the serial number [A].

Local API auth ([Nanoleaf auth docs](https://support.nanoleaf.me/hc/en-us/articles/41108368751892-API-Authentication-Security)) [A]: hold power 5–7 s (manual says 3 s — discrepancy), then `POST http://<ip>:16021/api/v1/new` within 30 s → `auth_token`. Token in the URL path on plain HTTP. "Tokens last until the device is reset"; `DELETE /api/v1/<token>` revokes. Details in [network-api.md](network-api.md).

## 5. GPL / open source

- No open-source notice, GPL offer or source download from Nanoleaf found. The Terms of Use prohibit reverse engineering "of the Website or any software or technology… forming part thereof" ([ToU](https://support.nanoleaf.me/hc/en-us/articles/36816871748500-T-C-Legal-Terms-of-Use-Policy)) [A].
- OpenWrt forum user (March 2023) quotes Nanoleaf: "Based on our use of GPL, we do not intend to release our source code or a contributor version at this time." ([thread](https://forum.openwrt.org/t/nanoleaf-light-panels/81748)) [B].
- The original forum.nanoleaf.me thread on OpenWrt licensing now redirects; content unavailable.

## 6. Gaps

- Canvas OS, kernel and OpenWrt version; bootloader; console baud rate and access.
- Firmware image format, signing, encryption, partition layout.
- Whether Canvas was affected by CVE-2022-47758 / CVE-2026-33268.
- Cloud endpoints contacted by Canvas specifically.
- Readable copies of: nanoleaf.me product-security page, Reddit threads, cvedetails.
