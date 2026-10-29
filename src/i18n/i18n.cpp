// ps5-discord - The interface language: Turkish (the source text) or English.
// Copyright (C) 2026 Desstann
// SPDX-License-Identifier: MIT
#include "i18n/i18n.hpp"

#include <atomic>
#include <string_view>
#include <unordered_map>

namespace i18n
{
namespace
{

std::atomic<int> current{int(Lang::en)};

constexpr std::pair<const char *, const char *> kEnglish[] = {
    // Sign-in and loading
    {"Tekrar hoş geldin!", "Welcome back!"},
    {"Seni tekrar gördüğümüze çok sevindik!", "We're so excited to see you again!"},
    {"BİLGİSAYARDAN GİRİŞ YAP", "SIGN IN FROM A COMPUTER"},
    {"Aynı ağdaki bir bilgisayarda bu adresi tarayıcıya yaz,",
     "On a computer on the same network, open this address,"},
    {"açılan sayfada e-posta ve şifrenle giriş yap.", "then sign in with your email and password."},
    {"Hazırlanıyor...", "Preparing..."},
    {"%s olarak giriş yapılıyor", "Signing in as %s"},
    {"QR Kodu ile Giriş Yap", "Log in with QR Code"},
    {"Anında giriş yapmak için Discord", "Scan this with the Discord mobile"},
    {"mobil uygulamasıyla tara.", "app to log in instantly."},
    {"Bağlanılıyor...", "Connecting..."},
    {"BİLİYOR MUYDUN", "DID YOU KNOW"},
    {"QR kodu hazırlanıyor...", "Preparing the QR code..."},
    {"Anahtar üretilemedi", "Could not create a key"},
    {"Bağlanılamadı: ", "Could not connect: "},
    {"QR kodunun süresi doldu, yenileniyor...", "The QR code expired, renewing..."},
    {"Telefonda girişi onayla", "Confirm the sign-in on your phone"},
    {"Giriş yapılıyor...", "Signing in..."},
    {"Giriş başarısız: ", "Sign-in failed: "},
    {"Giriş telefonda iptal edildi", "Sign-in was cancelled on the phone"},
    {"Discord'a bağlanılıyor...", "Connecting to Discord..."},
    {"Discord yanıtı bekleniyor...", "Waiting for Discord..."},
    {"Hesap yükleniyor...", "Loading your account..."},
    {"Discord yanıt vermedi", "Discord did not respond"},
    {"Hesap yüklenemedi", "Could not load your account"},
    {"Yeniden Dene", "Retry"},
    {"Oturum geçersiz, tekrar giriş gerekiyor", "Session expired, please sign in again"},
    {"Bağlantı koptu: ", "Connection lost: "},

    // The client
    {"Sohbet bul ya da başlat", "Find or start a conversation"},
    {"DİREKT MESAJLAR", "DIRECT MESSAGES"},
    {"Özel Mesajlar", "Direct Messages"},
    {"Çevrim içi", "Online"},
    {"Çevrim dışı", "Offline"},
    {"Aç", "Open"},
    {"Geri", "Back"},
    {"Sunucu", "Server"},
    {"Mesaj", "Message"},
    {"Ayarlar", "Settings"},
    {"Sesten Ayrıl", "Leave Voice"},
    {"Bir kanal seç", "Pick a channel"},
    {"Soldan bir sunucu ve kanal seçip aç.", "Choose a server and a channel on the left."},
    {"Forum gönderileri", "Forum posts"},
    {"Gönderiler yükleniyor...", "Loading posts..."},
    {"Bu forumda henüz gönderi yok.", "No posts in this forum yet."},
    {"%s yanıt", "%s replies"},
    {"%s gönderi", "%s posts"},
    {"Mesajlar yükleniyor...", "Loading messages..."},
    {"%s kanalına hoş geldin!", "Welcome to #%s!"},
    {"Bu, #%s kanalının başlangıcı.", "This is the start of the #%s channel."},
    {"%s ile olan direkt mesaj geçmişinin başlangıcı.",
     "This is the beginning of your direct message history with %s."},
    {"@%s kullanıcısına mesaj gönder", "Message @%s"},
    {"#%s kanalına mesaj gönder", "Message #%s"},
    {"Mesajını yaz...", "Type your message..."},
    {"Gönder", "Send"},
    {"Boşluk", "Space"},
    {"(düzenlendi)", "(edited)"},
    {" (düzenlendi)", " (edited)"},
    {"Eki görüntülemek için tıkla", "Click to see attachment"},
    {"bilinmeyen-kullanıcı", "unknown-user"},
    {"Dün saat %02d:%02d", "Yesterday at %02d:%02d"},
    {" sunucuya katıldı.", " joined the server."},
    {" bu kanala bir mesaj sabitledi.", " pinned a message to this channel."},
    {" sunucuya takviye yaptı!", " boosted the server!"},
    {" bir alt başlık başlattı: ", " started a thread: "},
    {" bir işlem yaptı.", " did something."},
    {"Mesaj gönderilemedi (%s)", "Could not send the message (%s)"},
    {"Bu kanalı görme iznin yok", "You don't have permission to see this channel"},
    {"Mesajlar yüklenemedi (%s)", "Could not load messages (%s)"},

    // Voice
    {"Ses Bağlandı", "Voice Connected"},
    {"Ses Bağlantısı Kesildi", "Voice Disconnected"},
    {"Bağlanıyor", "Connecting"},
    {"Sesi Aç", "Unmute"},
    {"Sessize Al", "Mute"},
    {"Ayrıl", "Leave"},
    {"Ses oturumu geçersiz oldu", "The voice session expired"},
    {"Ses kanalından çıkarıldın", "You were removed from the voice channel"},
    {"Ses sunucusu uçtan uca şifreleme istedi", "The voice server asked for end-to-end encryption"},
    {"Ses bağlantısı koptu (%s)", "Voice connection lost (%s)"},
    {"Ses sunucusuna bağlanılıyor...", "Connecting to the voice server..."},
    {"Ses sunucusuna bağlanılamadı: ", "Could not reach the voice server: "},
    {"Ses bağlantısı kuruldu, uçtan uca şifreli", "Voice connected, end-to-end encrypted"},
    {"Ses bağlantısı kuruldu", "Voice connected"},
    {"Ses için UDP soketi açılamadı", "Could not open a UDP socket for voice"},
    {"Ses sunucusunun adresi çözülemedi", "Could not resolve the voice server"},
    {"Ses sunucusuna UDP ile ulaşılamadı", "Could not reach the voice server over UDP"},
    {"Ses şifreleme anahtarı alınamadı", "Did not receive the voice encryption key"},
    {"Ses bağlandı, şifreleme anahtarları bekleniyor",
     "Voice connected, waiting for encryption keys"},
    {"Ses kodlayıcı açılamadı", "Could not start the voice encoder"},
    {"Ses hatası: ", "Voice error: "},
    {"Ses kanalına bağlanılıyor...", "Joining the voice channel..."},
    {"Ses iş parçacığı başlatılamadı", "Could not start voice"},
    {"Mikrofon bulunamadı", "No microphone found"},

    // Dialogs
    {"İptal", "Cancel"},
    {"Çıkış Yap", "Log Out"},
    {"Çıkış yapmak istediğine emin misin?", "Are you sure you want to log out?"},
    {"PSCord topluluğuna davet edildin", "You've been invited to the PSCord community"},
    {"%s Çevrimiçi", "%s Online"},
    {"%s Üye", "%s Members"},
    {"Buradan katılamadık. Telefonunla okutup katıl:",
     "We couldn't join from here. Scan it with your phone:"},
    {"Katıldın!", "Joined!"},
    {"Katılınıyor...", "Joining..."},
    {"Sunucuya Katıl", "Join Server"},
    {"Daha Sonra", "Later"},

    // Updates
    {"Güncellemeler denetleniyor...", "Checking for updates..."},
    {"PSCord %s indiriliyor", "Downloading PSCord %s"},
    {"PSCord %s kuruluyor", "Installing PSCord %s"},
    {"Güncelleme tamamlandı, yeniden başlatılıyor", "Update complete, restarting"},
    {"Tekrar denemek için X'e bas", "Press X to try again"},
    {"İndirme başarısız oldu. İnternet bağlantını kontrol et.",
     "The download failed. Check your internet connection."},
    {"İndirilen güncelleme bozuk çıktı.", "The downloaded update is damaged."},
    {"Uygulama klasörüne yazılamadı.", "Could not write to the app folder."},
    {"Güncelleme dosyası açılamadı.", "Could not open the update file."},
    {"Güncelleme kurulamadı.", "Could not install the update."},

    // The PC sign-in page (dc/web_page.hpp)
    {R"(<html lang="tr">)", R"(<html lang="en">)"},
    {R"(<title>PSCord - Giriş</title>)", R"(<title>PSCord - Sign in</title>)"},
    {R"(Hesabına giriş yap. Bilgiler doğrudan PS5'e gider, PSCord Discord'a kendisi bağlanır.)",
     R"(Sign in to your account. Your details go straight to the PS5, and PSCord connects to Discord itself.)"},
    {R"(E-posta veya telefon numarası)", R"(Email or phone number)"},
    {R"(>Şifre<)", R"(>Password<)"},
    {R"(İki adımlı doğrulama kodu)", R"(Two-factor authentication code)"},
    {R"(Token ile giriş yap)", R"(Sign in with a token)"},
    {R"(Token ile giriş)", R"(Sign in with a token)"},
    {R"(>Giriş yap<)", R"(>Log In<)"},
    {R"(Şifren bu bilgisayarda saklanmaz. PS5 yalnızca Discord'un verdiği oturumu tutar.)",
     R"(Your password is not stored on this computer. The PS5 only keeps the session Discord gives it.)"},
    {R"("Giriş başarısız")", R"("Sign-in failed")"},
    {R"(Giriş yapıldı. PS5 ekranına bakabilirsin.)", R"(Signed in. Check your PS5 screen.)"},
    {R"(Doğrulama yüklenemedi, sayfayı yenile.)",
     R"(The captcha could not load, refresh the page.)"},
    {R"(Giriş yapılıyor...)", R"(Signing in...)"},
    {R"(Discord bir doğrulama istiyor: kutuyu işaretle.)",
     R"(Discord wants a captcha: tick the box.)"},
    {R"(PS5'e ulaşılamadı. Uygulama açık mı?)", R"(Could not reach the PS5. Is the app open?)"},
    {R"(Token PS5'e gönderildi. PS5 ekranına bakabilirsin.)",
     R"(The token was sent to the PS5. Check your PS5 screen.)"},
    {R"((s.me ? s.me + " olarak" : "") + " PS5'te giriş yapıldı.")",
     R"("Signed in on PS5" + (s.me ? " as " + s.me : "") + ".")"},

    // The PC sign-in page (dc/web_page.hpp)
    {R"T(<html lang="tr">)T", R"T(<html lang="en">)T"},
    {R"T(<title>PSCord - Giriş</title>)T", R"T(<title>PSCord - Sign in</title>)T"},
    {R"T(Hesabına giriş yap. Bilgiler doğrudan PS5'e gider, PSCord Discord'a kendisi bağlanır.)T",
     R"T(Sign in to your account. Your details go straight to the PS5, and PSCord connects to Discord itself.)T"},
    {R"T(E-posta veya telefon numarası)T", R"T(Email or phone number)T"},
    {R"T(>Şifre<)T", R"T(>Password<)T"},
    {R"T(İki adımlı doğrulama kodu)T", R"T(Two-factor authentication code)T"},
    {R"T(Token ile giriş yap)T", R"T(Sign in with a token)T"},
    {R"T(Token ile giriş)T", R"T(Sign in with a token)T"},
    {R"T(>Giriş yap<)T", R"T(>Log In<)T"},
    {R"T(Şifren bu bilgisayarda saklanmaz. PS5 yalnızca Discord'un verdiği oturumu tutar.)T",
     R"T(Your password is not stored on this computer. The PS5 only keeps the session Discord gives it.)T"},
    {R"T("Giriş başarısız")T", R"T("Sign-in failed")T"},
    {R"T(Giriş yapıldı. PS5 ekranına bakabilirsin.)T", R"T(Signed in. Check your PS5 screen.)T"},
    {R"T(Doğrulama yüklenemedi, sayfayı yenile.)T",
     R"T(The captcha could not load, refresh the page.)T"},
    {R"T(Discord bir doğrulama istiyor: kutuyu işaretle.)T",
     R"T(Discord wants a captcha: tick the box.)T"},
    {R"T(PS5'e ulaşılamadı. Uygulama açık mı?)T", R"T(Could not reach the PS5. Is the app open?)T"},
    {R"T(Token PS5'e gönderildi. PS5 ekranına bakabilirsin.)T",
     R"T(The token was sent to the PS5. Check your PS5 screen.)T"},
    {R"T((s.me ? s.me + " olarak" : "") + " PS5'te giriş yapıldı.")T",
     R"T("Signed in on PS5" + (s.me ? " as " + s.me : "") + ".")T"},

    // Settings
    {"KULLANICI AYARLARI", "USER SETTINGS"},
    {"UYGULAMA AYARLARI", "APP SETTINGS"},
    {"Hesabım", "My Account"},
    {"Dil", "Language"},
    {"Ses ve Kontroller", "Sound & Controls"},
    {"Hakkında", "About"},
    {"KULLANICI ADI", "USERNAME"},
    {"Uygulamanın dilini seç.", "Choose the language of the app."},
    {"Arayüz sesleri", "Interface sounds"},
    {"Titreşim", "Vibration"},
    {"Işık çubuğu", "Light bar"},
    {"Onay için O, geri için X", "Circle confirms, Cross goes back"},
    {"Açık", "On"},
    {"Kapalı", "Off"},
    {"Sürüm", "Version"},
    {"Güncellemeler uygulama açılırken otomatik kurulur.",
     "Updates install automatically when the app opens."},
    {"Topluluğa katıl", "Join the community"},
    {"discord.gg/wfTch3qUa6", "discord.gg/wfTch3qUa6"},
    {"Bir kahve ısmarla", "Buy me a coffee"},
    {"buymeacoffee.com/desstann", "buymeacoffee.com/desstann"},
    {"Beğendiysen telefonunla okut.", "If you like it, scan this with your phone."},
    {"Sesli kanala yeniden bağlanılıyor", "Rejoining the voice channel"},
    {"%s seni etiketledi", "%s mentioned you"},
    {"%s mesaj gönderdi", "%s sent a message"},
    {"Seç", "Select"},
    {"Değiştir", "Change"},
    {"Kapat", "Close"},
};

const std::unordered_map<std::string_view, const char *> &english()
{
    static const std::unordered_map<std::string_view, const char *> table(std::begin(kEnglish),
                                                                          std::end(kEnglish));
    return table;
}

constexpr const char *kMonths[2][12] = {
    {"Ocak", "Şubat", "Mart", "Nisan", "Mayıs", "Haziran", "Temmuz", "Ağustos", "Eylül", "Ekim",
     "Kasım", "Aralık"},
    {"January", "February", "March", "April", "May", "June", "July", "August", "September",
     "October", "November", "December"},
};
constexpr const char *kWeekdays[2][7] = {
    {"Pazar", "Pazartesi", "Salı", "Çarşamba", "Perşembe", "Cuma", "Cumartesi"},
    {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"},
};

} // namespace

void set_language(Lang lang)
{
    current.store(int(lang));
}

Lang language()
{
    return Lang(current.load());
}

const char *language_name(Lang lang)
{
    return lang == Lang::en ? "English" : "Türkçe";
}

const char *T(const char *turkish)
{
    if (language() == Lang::tr)
        return turkish;
    const auto &table = english();
    const auto found = table.find(turkish);
    return found == table.end() ? turkish : found->second;
}

std::string T(const char *turkish, const std::string &arg)
{
    std::string text = T(turkish);
    const size_t at = text.find("%s");
    if (at != std::string::npos)
        text.replace(at, 2, arg);
    return text;
}

const char *month_name(int month)
{
    return kMonths[int(language())][month % 12];
}

const char *weekday_name(int day)
{
    return kWeekdays[int(language())][day % 7];
}

} // namespace i18n
