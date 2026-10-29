// ps5-discord - The sign-in page the console serves to a PC on the same network.
// Copyright (C) 2026 Desstann
// SPDX-License-Identifier: MIT
#pragma once

namespace dc::web {

inline constexpr const char* kSignInPage = R"HTML(<!doctype html>
<html lang="tr">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>PSCord - Giriş</title>
<style>
  * { box-sizing: border-box; }
  body { margin: 0; min-height: 100vh; display: grid; place-items: center; background: #5865f2;
         font-family: "gg sans", "Segoe UI", Roboto, "Helvetica Neue", Arial, sans-serif; color: #f2f3f5; }
  .card { width: min(480px, calc(100vw - 32px)); background: #313338; border-radius: 8px; padding: 32px;
          box-shadow: 0 8px 24px rgba(0,0,0,.35); }
  h1 { margin: 0 0 8px; font-size: 24px; text-align: center; }
  .sub { margin: 0 0 24px; color: #b5bac1; text-align: center; font-size: 15px; }
  label { display: block; margin: 16px 0 8px; font-size: 12px; font-weight: 700; letter-spacing: .02em;
          text-transform: uppercase; color: #b5bac1; }
  input { width: 100%; padding: 10px; border: 0; border-radius: 4px; background: #1e1f22; color: #f2f3f5;
          font-size: 16px; outline: none; }
  input:focus { box-shadow: 0 0 0 2px #5865f2; }
  button { width: 100%; margin-top: 24px; padding: 12px; border: 0; border-radius: 4px; background: #5865f2;
           color: #fff; font-size: 16px; font-weight: 600; cursor: pointer; }
  button:hover { background: #4752c4; }
  button:disabled { opacity: .6; cursor: progress; }
  .msg { margin-top: 16px; min-height: 20px; font-size: 14px; color: #fa777c; }
  .msg.ok { color: #23a55a; }
  .captcha { display: flex; justify-content: center; margin-top: 20px; }
  .hidden { display: none; }
  details { margin-top: 28px; color: #b5bac1; font-size: 14px; }
  summary { cursor: pointer; }
  .done { text-align: center; font-size: 18px; }
  .note { margin-top: 24px; font-size: 12px; color: #949ba4; text-align: center; }
</style>
</head>
<body>
<div class="card">
  <h1>PSCord</h1>
  <p class="sub">Hesabına giriş yap. Bilgiler doğrudan PS5'e gider, PSCord Discord'a kendisi bağlanır.</p>

  <form id="login">
    <label for="email">E-posta veya telefon numarası</label>
    <input id="email" autocomplete="username" required>
    <label for="password">Şifre</label>
    <input id="password" type="password" autocomplete="current-password" required>
    <div id="captcha" class="captcha hidden"></div>
    <button id="send" type="submit">Giriş yap</button>
  </form>

  <form id="mfa" class="hidden">
    <label for="code">İki adımlı doğrulama kodu</label>
    <input id="code" inputmode="numeric" autocomplete="one-time-code" placeholder="6 haneli kod veya yedek kod" required>
    <button type="submit">Onayla</button>
  </form>

  <div id="done" class="done hidden"></div>
  <div id="msg" class="msg"></div>

  <details id="tokenbox">
    <summary>Token ile giriş</summary>
    <form id="token">
      <label for="tokenvalue">Discord token</label>
      <input id="tokenvalue" type="password">
      <button type="submit">Token ile giriş yap</button>
    </form>
  </details>
  <p class="note">Şifren bu bilgisayarda saklanmaz. PS5 yalnızca Discord'un verdiği oturumu tutar.</p>
</div>

<script src="https://js.hcaptcha.com/1/api.js?render=explicit" async defer></script>
<script>
const $ = (id) => document.getElementById(id);
let ticket = "";

function show(text, ok) {
  $("msg").textContent = text || "";
  $("msg").className = ok ? "msg ok" : "msg";
}

function discordError(r) {
  const e = r.errors || {};
  for (const field of ["login", "password", "code", "token"]) {
    const list = e[field] && e[field]._errors;
    if (list && list.length) return list[0].message;
  }
  return r.message || "Giriş başarısız";
}

async function post(path, body) {
  const res = await fetch(path, { method: "POST", headers: { "Content-Type": "application/json" },
                                  body: JSON.stringify(body) });
  return res.json();
}

function finished() {
  ["login", "mfa", "tokenbox"].forEach((id) => $(id).classList.add("hidden"));
  $("done").classList.remove("hidden");
  $("done").textContent = "Giriş yapıldı. PS5 ekranına bakabilirsin.";
  show("");
}

async function handle(r) {
  if (r.ok) return finished();
  if (r.mfa && r.ticket) {
    ticket = r.ticket;
    $("login").classList.add("hidden");
    $("mfa").classList.remove("hidden");
    $("code").focus();
    return show("");
  }
  if (r.captcha_sitekey) {
    if (typeof hcaptcha === "undefined") return show("Doğrulama yüklenemedi, sayfayı yenile.");
    const box = $("captcha");
    box.innerHTML = "";
    box.classList.remove("hidden");
    const id = hcaptcha.render(box, {
      sitekey: r.captcha_sitekey,
      theme: "dark",
      callback: async (key) => {
        show("Giriş yapılıyor...", true);
        box.classList.add("hidden");
        await handle(await post("/api/login", { login: $("email").value, password: $("password").value,
          captcha_key: key, captcha_rqtoken: r.captcha_rqtoken || "", captcha_session_id: r.captcha_session_id || "" }));
      },
    });
    if (r.captcha_rqdata) hcaptcha.setData(id, { rqdata: r.captcha_rqdata });
    return show("Discord bir doğrulama istiyor: kutuyu işaretle.", true);
  }
  show(discordError(r));
}

$("login").addEventListener("submit", async (ev) => {
  ev.preventDefault();
  $("send").disabled = true;
  show("Giriş yapılıyor...", true);
  try {
    await handle(await post("/api/login", { login: $("email").value, password: $("password").value }));
  } catch (e) {
    show("PS5'e ulaşılamadı. Uygulama açık mı?");
  }
  $("send").disabled = false;
});

$("mfa").addEventListener("submit", async (ev) => {
  ev.preventDefault();
  show("Kontrol ediliyor...", true);
  try {
    await handle(await post("/api/mfa", { code: $("code").value.replace(/\s/g, ""), ticket }));
  } catch (e) {
    show("PS5'e ulaşılamadı. Uygulama açık mı?");
  }
});

$("token").addEventListener("submit", async (ev) => {
  ev.preventDefault();
  const r = await post("/api/token", { token: $("tokenvalue").value.trim() });
  if (r.ok) {
    $("done").classList.remove("hidden");
    $("done").textContent = "Token PS5'e gönderildi. PS5 ekranına bakabilirsin.";
    ["login", "mfa", "tokenbox"].forEach((id) => $(id).classList.add("hidden"));
    show("");
  } else {
    show(discordError(r));
  }
});

setInterval(async () => {
  try {
    const s = await (await fetch("/api/state")).json();
    if (s.state === "ready") {
      ["login", "mfa", "tokenbox"].forEach((id) => $(id).classList.add("hidden"));
      $("done").classList.remove("hidden");
      $("done").textContent = (s.me ? s.me + " olarak" : "") + " PS5'te giriş yapıldı.";
      show("");
    }
  } catch (e) {}
}, 3000);
</script>
</body>
</html>
)HTML";

// The page's text as written above, served through i18n::T() in the app's language.
inline constexpr const char* kSignInTexts[] = {
    R"T(<html lang="tr">)T",
    R"T(<title>PSCord - Giriş</title>)T",
    R"T(Hesabına giriş yap. Bilgiler doğrudan PS5'e gider, PSCord Discord'a kendisi bağlanır.)T",
    R"T(E-posta veya telefon numarası)T",
    R"T(>Şifre<)T",
    R"T(İki adımlı doğrulama kodu)T",
    R"T(Token ile giriş yap)T",
    R"T(Token ile giriş)T",
    R"T(>Giriş yap<)T",
    R"T(Şifren bu bilgisayarda saklanmaz. PS5 yalnızca Discord'un verdiği oturumu tutar.)T",
    R"T("Giriş başarısız")T",
    R"T(Giriş yapıldı. PS5 ekranına bakabilirsin.)T",
    R"T(Doğrulama yüklenemedi, sayfayı yenile.)T",
    R"T(Giriş yapılıyor...)T",
    R"T(Discord bir doğrulama istiyor: kutuyu işaretle.)T",
    R"T(PS5'e ulaşılamadı. Uygulama açık mı?)T",
    R"T(Token PS5'e gönderildi. PS5 ekranına bakabilirsin.)T",
    R"T((s.me ? s.me + " olarak" : "") + " PS5'te giriş yapıldı.")T",
};

}  // namespace dc::web
