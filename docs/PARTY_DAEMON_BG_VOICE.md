# PartyDaemon arka plan Discord sesi

PSCord açıkken Discord sesi uygulamada sorunsuz. Amaç: home menü / başka oyun üstünde de duyulabilir kalsın. Bu dosya, denenen yolları, kanıtlanan kısıtları ve sıradaki mimari kararı toplar.

**Durum (2026-10-27 gece / rehber güncellemesi):** Menü cızırtısı + Close paniği sonrası teşhis netleşti: asıl sorun ince ayar değil, **ptrace üzerinden uzaktan `sceAudioOutOutput`**. Soft_pace / handle leak / cızırtı / Close çökmesi büyük ölçüde bu kırılganlıktan geliyor. Yarın önce süre ölçümü (karar testi), sonra grain büyütme veya process-local inject.

---

## 0. Mimari karar (okuma özeti)

### Teşhis

Uzaktan ptrace ile Output çağırmak gerçek zamanlı ses için yapısal olarak kırılgan:

- Her grain: attach veya stop → copyin → remote call → continue (ms mertebesi jitter).
- Grain bütçesi 256 @ 48 kHz ≈ **5.33 ms**. Bunu aşan Output → underrun / drop / soft_pace karmaşası.
- Open/Close başka süreçten → Close panik, idle open → menü glitch (veya underrun hipotezi).
- soft_pace, yavaş ptrace’i “tempo” ile telafi etmeye çalışır; idle’de menüyü bozar.

### Yarın — karar verici test

Pump’ta **her ptrace’li Output** süresini logla (us):

- Ortalama veya sık sık **> 5.33 ms** → cızırtı ince ayarla çözülmez; mimari değişmeli (inject).
- Sürekli **≪ 5.33 ms** → grain büyütme + idle politikası ile devam denenebilir (yine de inject kalıcı hedef).

Ölçüm: `clock_gettime` / tick sayacı around `pt_call_bp(... fn_out ...)`. İlk 50 grain + her 100. grain log; histogram (p50/p95/max) yeterli.

### Hızlı kazanç (mevcut ptrace yolu)

MAIN’i **1024 frame** ile aç (~21.3 ms). Saniyedeki ptrace çağrısı **4× azalır**. Voice chat için ~20 ms ek gecikme kabul edilebilir. Open’da desteklenen `len` (256’nın katları: 256/512/1024/…) doğrulanmalı; fail olursa bir alta düş.

### Kalıcı çözüm: PartyDaemon içinde yerel thread

ptrace **yalnızca bir kez kurulumda**:

1. PartyDaemon’a küçük PIC shim mmap + import tablosu + SPSC ring.
2. `scePthreadCreate` ile thread başlat → **hemen detach**.
3. Thread kendi sürecinden `sceAudioOutOpen/Output/Close` çağırır; bg-voice (veya feeder) ring’e `kernel_proc_copyin` / shared mapping yazar.
4. Underrun → sıfır grain çal (port beslenmiş kalır). Overrun → en eski slotu at (SPSC).
5. soft_pace kalkar (HW Output temposu).
6. Close aynı süreçten → muhtemelen güvenli; handle leak biter.

**Menü cızırtısı hipotezi (doğrulanacak):** sorun “açık port” değil, **beslenmeyen (underrun) açık port**. Inject thread idle’de silence Output ederse menü temiz kalmalı. Karşı test: ptrace yolunda idle’de silence besle vs portu hiç açma.

**Risk:** Enjekte thread çökerse PartyDaemon (sistem partisi) gider. Shim minimal: ring oku + Output; malloc/log yok. PartyDaemon title geçişinde ölürse `ensure_party_pump` → yeniden enjekte.

### Repoda zaten var (iptal edilmiş deneme)

| Dosya | Ne |
|--------|-----|
| `inject/pump_shim.c` + `pump_shim_abi.h` | PIC shim: MAIN open, SPSC ring, underrun=silence, local Output, Close on stop |
| `payload/party-beep-thread.c` | PartyDaemon’a `scePthreadCreate` stager (beep shim) |
| `payload/party-beep-hijack.c` | Alternatif hijack denemesi |
| `inject/beep_shim*` / `shellcore-inject.c` | Aynı inject kalıbı (ShellCore mic) |

`pump_shim` todo’su daha önce **“pthread inject failed on PartyDaemon”** diye iptal edildi; ptrace-per-grain’e düşüldü. Kalıcı yol: bu inject’i **neden fail olduğunu loglayarak** yeniden canlandırmak (LWP seçimi, attr, unlock_syscall_filter, CRT’siz entry).

### Dış referanslar

- [Usermode ELF injection on PS5](https://reversing.codes/posts/PlayStation-5-ELF-Injection/) — mmap ELF + `pthread_create` stager + `int3` + detach (elfldr RFI).
- [HackTricks: PS5 RFI / threaded stager](https://hacktricks.wiki/en/binary-exploitation/freebsd-ptrace-rfi-vm_map-prot_exec-bypass-ps5.html) — aynı kalıp.
- [Lapy JB Daemon / libhijacker](https://github.com/ArkSama/PS5-Lapy-JB-Daemon) — etaHEN’siz **ucred bump** (thread inject değil); hijacker offset / process walk referansı.
- PS5Stream: AgcCompositor / GameLiveStreaming içinde process-local kod (13.60’a özel streaming yolu) — aynı “kurulumda bir kez enjekte, sonra yerel çalış” fikri; yerel clone yoksa akışı yukarıdaki inject makalesi + mevcut `pump_shim` karşılar.

---

## 1. Mimari (ne neye bağlı)

```
Discord UDP/Opus
       │
       ▼
┌──────────────────┐   TCP :9191    ┌─────────────────────┐
│  bg-voice.elf    │◄──────────────►│  PSCord (RELAY)     │  ← uygulama içi oynatma
│  (mixer+engine)  │                └─────────────────────┘
│                  │   TCP :9191    ┌─────────────────────┐
│  tap: relay_grain│───────────────►│ party-audio-pump.elf│
└──────────────────┘   PUMP PCM     │ ptrace → PartyDaemon│
                                    │ sceAudioOut MAIN    │
                                    └─────────────────────┘
                                              │
                                              ▼
                                         TV / hoparlör
                                    (home + oyun üstü)
```

| Parça | Yol | Rol |
|--------|-----|-----|
| `bg-voice.elf` | `payload/bg-voice.cpp` | Privileged daemon: Opus, mixer, :9191 kontrol |
| `party-audio-pump.elf` | `payload/party-audio-pump.c` | PartyDaemon’a ptrace ile MAIN Output |
| App relay | `src/voice/bg_client.cpp` | Uygulama ayaktayken RELAY; ring stall → kapat → pump |
| Soft pace | `src/platform/ps5/audio_out.cpp` | Pump yavaşken mixer’ı HW yerine `sleep_us` ile temposlar |

Protokol v4: `PING` / `RELAY` / `PUMP ScePartyDaemon` / `STATUS` (`pump=0|1`).

Pump ELF yolları (tercih sırası): `/data/...` kopyası → app payload. `ensure_party_pump` STATUS’ta `pump=1` yoksa elfldr’a yollar.

Konsol: `$PS5_HOST` (ör. `192.168.1.100`) — FTP `2121`, elfldr `9021`. Loglar: `/data/ps5discord/bg-voice.log`, `party-audio-pump.log`.

---

## 2. Neden PartyDaemon MAIN?

PS5’te payload’ın kendi `sceAudioOut` portu (BGM/VOICE) çoğu zaman TV mix’inde **duyulmaz** (özellikle başka title focus’tayken). Focus attach (`sceAudioOutAttachToApplicationByPid`) da güvenilir değil (`0x809b00ff` vb.).

Denenen hedefler:

| Hedef | Open/Output | TV’de duyulur mu? | Not |
|--------|-------------|-------------------|-----|
| **ScePartyDaemon MAIN (port 0)** | Evet | **Evet** | Tek çalışan arka plan yolu |
| PartyDaemon BGM (1) | Evet | Hayır / zayıf | “Output OK” ama sessiz |
| SceVoiceAndAgent | Evet | Hayır | Log’da success, TV sessiz |
| SceDiscordDaemon | Evet | Hayır | Aynı |
| SceShellUI | Evet | Evet | **SIGSTOP → oyun/launch donuyor** — yasak |

Sonuç: hedef yalnızca `ScePartyDaemon`, port yalnızca **MAIN (0)**. Multi-port open + uzun bip konsolu hard-crash etti.

---

## 3. RELAY vs PUMP (çifte çalma yok)

1. **PSCord açık:** App `RELAY` bağlar → sadece app oynatır. Pump varsa **bilinçli drop** (`relay attached — pump dropped`) — PartyDaemon üzerinde ptrace tutmak title launch’ta panik.
2. **App çıkınca / ring stall:** RELAY kapanır → grain’ler PUMP’a.
3. **Pump yoksa:** Soft_pace kapalı; local AudioOut HW temposu (arka planda çoğu zaman inaudible).

`MSG_DONTWAIT` ile pump’a gönder: ptrace Output realtime’dan yavaş; bloklamak mixer’ı dondurur. Drop = tempo korunur, catch-up scrap’leri “robot/cızırtı” yapıyordu.

---

## 4. Ptrace kuralları (kanıtlanmış)

| Kural | Neden |
|--------|--------|
| Attach yalnızca konuşurken | Sürekli attach: launch crash; her grain attach/detach: cızırtı |
| Output sonrası hemen `PT_CONTINUE` | Stop’ta bırakmak PartyDaemon’ı wedge eder |
| Setup sonrası **mutlaka detach** | Ptrace tutarak PSCord/oyun açmak → konsol kapanır |
| ShellUI’ya dokunma | Stop → home↔game donması |
| `PT_DETACH` yalnızca tracer’dan | Başka process’ten DETACH hang; unstick = `SIGCONT` only |
| Çift SIGSTOP yok | Zaten stopped iken tekrar STOP → sonsuz wait |
| `waitpid` sonsuz blok yok | WNOHANG + timeout; eski hang soft_pace void’una yol açtı |
| Orbis return = **int32** | `long >= 0` hata kodlarını (`0x80260011`) handle sanır |

Grain: 256 frame × 48 kHz stereo s16 ≈ 5.33 ms.

---

## 5. Menü cızırtısı (son gece bulgusu)

**Semptom:** PSCord’a girmeden, home menüde gelip giden cızırtı.

**Kök neden adayları (ikisi de gerçek):**

1. **Idle açık MAIN** — Setup’ta bip için MAIN açılıp bırakılıyordu. PartyDaemon’da orphan MAIN, sistem ses yolunu bozuyor.
2. **Pump bağlanınca soft_pace ON** — Discord yokken bile paced silence → local BGM + sürekli silence grain → pump. Menüde glitch.

**Yapılan düzeltmeler (kodda, redeploy bekliyor):**

- Pump: setup’ta MAIN **açma / bip yok**; sembol + mmap + detach. MAIN yalnızca `peak > 64` konuşmada.
- Pump: idle ~200 ms paket yok → handle abandon + detach.
- bg-voice: soft_pace yalnızca audible grain (veya ~320 ms hangover) pump’a giderken.
- bg-voice: pump connect’te soft_pace **açma**.
- Hangover: hece arası MAIN open/close yapmasın diye ~60 grain sessizlik hâlâ gönderilir.

**`sceAudioOutClose` yasak (ptrace ile):** Close denemesi konsolu düşürdü. Handle’ı close etme; idle’de referansı bırak (`abandon_main`), detach et. Leak riski var (çok oturum) ama Close panik’ten daha iyi.

**Güncel hipotez:** Menü cızırtısı “açık port”tan değil, **underrun’lu açık port**tan geliyor olabilir. Inject thread idle’de sıfır grain basarsa ayrışır; ptrace yolunda da “idle silence besle” vs “hiç açma” A/B’si bunu test eder.

---

## 6. Deneme geçmişi (kısa)

| Deneme | Sonuç |
|--------|--------|
| Periyodik detach (~2 s) | Konuşma kesik kesik |
| Her grain attach/detach | Cızırtı |
| Catch-up usleep / drain | Robot / noise |
| Soft_pace + ölü pump | Ses kaybı (void) |
| Soft_pace ↔ pump couple | Gerekli (konuşma sırasında) |
| BGM / VoiceAndAgent / DiscordDaemon | TV sessiz |
| ShellUI pump | Launch freeze |
| Multi-port + uzun bip | Hard crash |
| Ptrace tutarak PSCord aç | Konsol kapanır |
| RELAY gelince pump drop | Launch güvenliği |
| MAIN-only kısa bip + detach | Bip OK; idle MAIN → menü cızırtı |
| Close after beep | Konsol drop |
| Soft_pace only on speech + lazy MAIN | Kodlandı; yarın doğrulanacak |

---

## 7. Dosya haritası

```
payload/bg-voice.cpp          daemon, RELAY/PUMP, soft_pace gate, hangover
payload/party-audio-pump.c    ptrace MAIN pump (lazy open, abandon, no Close)
src/voice/bg_client.cpp       ensure_running / ensure_party_pump
src/platform/ps5/audio_out.*  set_soft_pace, tap, Output thread
src/voice/bg_protocol.hpp     kVersion, ELF path sabitleri
tools/build.sh                party-audio-pump.elf
tools/build-bg-voice.sh       bg-voice.elf
```

Log anahtarları:

- Pump: `detached after setup`, `PUMP connected`, `reopen MAIN=… for speech`, `abandon MAIN + detach after idle`
- bg-voice: `party pump attached`, `relay: app is playing` / `app gone, party pump`, `relay attached — pump dropped`

---

## 8. Deploy (hatırlatma)

```bash
# Build
export LLVM_CONFIG=/opt/homebrew/opt/llvm@18/bin/llvm-config
export PATH="/opt/homebrew/opt/llvm@18/bin:/opt/homebrew/bin:$PATH"
sdk=.../app/.deps/native/ps5-payload-sdk
root=.../app
"$sdk/bin/prospero-clang" -O2 -o "$root/payload/party-audio-pump.elf" \
    "$root/payload/party-audio-pump.c"
bash "$root/tools/build-bg-voice.sh" "$root/payload/bg-voice.elf"
```

FTP → `/data/ps5discord/` + `/data/homebrew/PPSA99190/payload/`. Önce eski `payload.elf` / `party-audio*` / `bg-voice*` kill + PartyDaemon `SIGCONT`, sonra bg-voice, sonra pump.

**Push yok** (kullanıcı istemedikçe). Commit author kuralları PKG Folderizer’a özel; bu repo için normal akış.

---

## 9. Yarın — checklist (öncelik sırası)

### A. Karar testi (önce)

1. Konsol + FTP/elfldr ayakta.
2. Pump’a Output süresi logu ekle → deploy → kısa konuşma (menü veya in-app handoff).
3. p50 / p95 / max **vs 5.33 ms** kaydet.
4. Eşik aşımı sık → **B atla, C’ye geç**. Nadir aşım → B dene.

### B. Hızlı kazanç (ptrace hâlâ ayaktaysa)

1. MAIN open `len=1024` (gerekirse 512 fallback).
2. Feeder grain boyutu ile hizala (bg-voice tap / pump `kFrames`).
3. Menü idle + konuşma + PSCord gir/çık crash yok mu?

### C. Kalıcı: inject thread (asıl hedef)

1. `inject/pump_shim` + stager’ı yeniden ayağa kaldır; fail noktasını logla (`scePthreadCreate` rc, LWP, magic/alive).
2. Feeder: ring’e yaz, **ptrace Output yok**; soft_pace kaldır.
3. Underrun = silence Output → menü cızırtısı hipotezini doğrula.
4. Stop → aynı süreçten Close; handle leak yok.
5. PartyDaemon restart → `ensure_party_pump` yeniden inject.
6. RELAY gelince shim `stop=1` veya feeder sustur (çift çalma yok); ptrace tutma.

### D. Eski idle-fix doğrulaması (opsiyonel, A/B yanında)

Lazy-MAIN + soft_pace-on-speech deploy: menü sessiz mi? (Close kullanma.)

Mic / ekran paylaşımı bu fazın dışında.

---

## 10. Bilinçli kararlar (özet)

- Duyulabilir arka plan = **PartyDaemon MAIN** (başka target yok).
- **Kalıcı oynatma yolu** = process-local thread + SPSC ring; ptrace sadece kurulum.
- Uygulama içi = **sadece RELAY**; arka plan shim/pump aynı anda çalmaz.
- ptrace-per-grain = geçici / ölçüm + grain büyütme ile sınırlı.
- Close’a **ptrace ile** dokunma; Close yalnızca hedef süreç içinden.
- ShellUI yasak; multi-port yasak; uzun beep yasak.
- Enjekte thread minimal (malloc/log yok); çökme = PartyDaemon kaybı.
