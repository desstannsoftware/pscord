// ps5-discord - Voice channel listener: voice gateway, UDP media, DAVE and Opus.
// Copyright (C) 2026 Desstann
// SPDX-License-Identifier: MIT
//
// Follows Discord's voice connection guide (gateway v8) and libdave's
// DaveSessionManager sample.

#include "voice/engine.hpp"
#include "i18n/i18n.hpp"

#include <arpa/inet.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <variant>
#include <vector>

#include <dave/dave_interfaces.h>
#include <dave/version.h>
#include <nlohmann/json.hpp>
#include <opus.h>
#include <sodium.h>

#include "audio/stream_ring.hpp"
#include "dc/net/dns.hpp"
#include "dc/net/websocket.hpp"
#include "voice/mic.hpp"

extern "C" __attribute__((weak)) int sceSystemServiceGetStatus(void* status);

namespace voice {

using json = nlohmann::json;
using Clock = std::chrono::steady_clock;
namespace dave = discord::dave;

namespace {

constexpr int kFrame = 960;            // 20 ms at 48 kHz
constexpr size_t kMaxQueued = 48000;   // stereo samples per speaker (0.5 s) before old audio is dropped
constexpr size_t kTargetBuffered = 1920; // frames kept ahead of the mixer (40 ms)
constexpr float kVoiceLevel = 0.012f;    // RMS above which the microphone counts as speech
constexpr int kHangFrames = 15;          // keep sending 300 ms after speech stops

uint16_t be16(const uint8_t* p) {
    return uint16_t((p[0] << 8) | p[1]);
}

uint32_t be32(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

void put16(uint8_t* p, uint16_t v) {
    p[0] = uint8_t(v >> 8);
    p[1] = uint8_t(v);
}

void put32(uint8_t* p, uint32_t v) {
    p[0] = uint8_t(v >> 24);
    p[1] = uint8_t(v >> 16);
    p[2] = uint8_t(v >> 8);
    p[3] = uint8_t(v);
}

long long now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string close_reason(int code, const std::string& error) {
    switch (code) {
    case 4006:
        return i18n::T("Ses oturumu geçersiz oldu");
    case 4014:
        return i18n::T("Ses kanalından çıkarıldın");
    case 4017:
        return i18n::T("Ses sunucusu uçtan uca şifreleme istedi");
    default:
        return i18n::T("Ses bağlantısı koptu (%s)", error);
    }
}

} // namespace

struct Engine::Impl {
    mutable std::mutex lock;
    Status status;
    hui::audio::StreamRing* ring = nullptr;
    std::atomic<bool> stop{false};
    std::atomic<bool> muted{false};
    pthread_t thread{};
    bool running = false;
    JoinInfo info;

    void set(Phase phase, const std::string& text, bool e2ee = false) {
        std::lock_guard<std::mutex> hold(lock);
        status.phase = phase;
        status.text = text;
        status.e2ee = e2ee;
        if (!text.empty()) {
            std::printf("[voice] %s\n", text.c_str());
        }
    }

    void set_speaking(std::set<std::string> users) {
        std::lock_guard<std::mutex> hold(lock);
        status.speaking = std::move(users);
    }

    void set_mic(bool open, const std::string& error) {
        std::lock_guard<std::mutex> hold(lock);
        status.mic = open;
        status.mic_error = error;
    }
};

namespace {

class Connection {
public:
    Connection(Engine::Impl& owner, JoinInfo info) : owner_(owner), info_(std::move(info)) {}

    ~Connection() {
        mic_.close();
        if (encoder_) {
            opus_encoder_destroy(encoder_);
        }
        if (udp_ >= 0) {
            ::close(udp_);
        }
        for (auto& [ssrc, source] : sources_) {
            opus_decoder_destroy(source.decoder);
        }
    }

    void run() {
        owner_.set(Phase::connecting, i18n::T("Ses sunucusuna bağlanılıyor..."));
        if (!ws_.connect("wss://" + info_.endpoint + "/?v=8")) {
            owner_.set(Phase::failed, i18n::T("Ses sunucusuna bağlanılamadı: ") + ws_.error());
            return;
        }
        send_json({{"op", 0},
                   {"d",
                    {{"server_id", info_.server_id},
                     {"user_id", info_.user_id},
                     {"session_id", info_.session_id},
                     {"token", info_.token},
                     {"max_dave_protocol_version", dave::MaxSupportedProtocolVersion()}}}});
        while (!owner_.stop.load()) {
            std::string message;
            const auto result = ws_.receive(message, 5);
            if (result == dc::net::WebSocket::Result::closed) {
                owner_.set(Phase::failed, close_reason(ws_.close_code(), ws_.error()));
                break;
            }
            if (result == dc::net::WebSocket::Result::message) {
                if (ws_.binary()) {
                    on_binary(message);
                } else {
                    on_text(message);
                }
            }
            heartbeat();
            pump_udp();
            send_mic();
            mix();
            keepalive();
            report_speaking();
            log_stats();
        }
        ws_.close();
    }

private:
    // ---- voice gateway ------------------------------------------------------

    void send_json(const json& message) { ws_.send_text(message.dump()); }

    void send_binary(uint8_t opcode, const std::vector<uint8_t>& payload) {
        std::string frame(1, char(opcode));
        frame.append(reinterpret_cast<const char*>(payload.data()), payload.size());
        ws_.send_binary(frame);
    }

    void heartbeat() {
        if (beat_ms_ <= 0 || Clock::now() < next_beat_) {
            return;
        }
        send_json({{"op", 3}, {"d", {{"t", now_ms()}, {"seq_ack", seq_}}}});
        next_beat_ = Clock::now() + std::chrono::milliseconds(beat_ms_);
    }

    void on_text(const std::string& text) {
        json message = json::parse(text, nullptr, false);
        if (message.is_discarded() || !message.is_object()) {
            return;
        }
        if (message.contains("seq") && message["seq"].is_number_integer()) {
            seq_ = message["seq"].get<int>();
        }
        const int op = message.value("op", -1);
        const json d = message.contains("d") ? message["d"] : json();
        switch (op) {
        case 8: // hello
            beat_ms_ = int(d.value("heartbeat_interval", 13750.0));
            next_beat_ = Clock::now();
            break;
        case 2: // ready
            on_ready(d);
            break;
        case 4: // session description
            on_session(d);
            break;
        case 5: // speaking: who sends on which ssrc
            if (d.contains("user_id") && d.contains("ssrc")) {
                ssrc_users_[d["ssrc"].get<uint32_t>()] = d["user_id"].get<std::string>();
            }
            break;
        case 11: // clients connect
            for (const auto& id : d.value("user_ids", json::array())) {
                const std::string user = id.get<std::string>();
                recognized_.insert(user);
                setup_ratchet(user, latest_version_);
            }
            break;
        case 13: { // client disconnect
            const std::string user = d.value("user_id", std::string());
            recognized_.erase(user);
            decryptors_.erase(user);
            break;
        }
        case 21: // DAVE prepare transition
            prepare_ratchets(uint16_t(d.value("transition_id", 0)), d.value("protocol_version", 0));
            ready_for(uint16_t(d.value("transition_id", 0)));
            break;
        case 22: // DAVE execute transition
            execute_transition(uint16_t(d.value("transition_id", 0)));
            break;
        case 24: { // DAVE prepare epoch
            if (!d.contains("epoch")) {
                break;
            }
            const json& epoch = d.at("epoch");
            const long long value = epoch.is_string() ? std::stoll(epoch.get<std::string>()) : epoch.get<long long>();
            if (value == 1) {
                init_group(d.value("protocol_version", 0));
                send_key_package();
            }
            break;
        }
        default:
            break;
        }
    }

    void on_binary(const std::string& data) {
        if (data.size() < 3) {
            return;
        }
        const auto* bytes = reinterpret_cast<const uint8_t*>(data.data());
        seq_ = be16(bytes);
        const uint8_t op = bytes[2];
        std::vector<uint8_t> payload(bytes + 3, bytes + data.size());
        if (!mls_) {
            mls_ = dave::mls::CreateSession(nullptr, "", [](std::string const& source, std::string const& reason) {
                std::printf("[voice] MLS failure %s: %s\n", source.c_str(), reason.c_str());
            });
        }
        switch (op) {
        case 25: // external sender
            mls_->SetExternalSender(payload);
            break;
        case 27: // proposals
            if (auto commit = mls_->ProcessProposals(payload, recognized_with_self())) {
                send_binary(28, *commit);
            }
            break;
        case 29: { // announce commit transition
            if (payload.size() < 2) {
                return;
            }
            const uint16_t transition = be16(payload.data());
            auto roster = mls_->ProcessCommit(std::vector<uint8_t>(payload.begin() + 2, payload.end()));
            if (std::holds_alternative<dave::ignored_t>(roster)) {
                return;
            }
            if (std::holds_alternative<dave::RosterMap>(roster)) {
                prepare_ratchets(transition, mls_->GetProtocolVersion());
                ready_for(transition);
            } else {
                send_json({{"op", 31}, {"d", {{"transition_id", transition}}}});
                dave_init(mls_->GetProtocolVersion());
            }
            break;
        }
        case 30: { // welcome
            if (payload.size() < 2) {
                return;
            }
            const uint16_t transition = be16(payload.data());
            auto roster = mls_->ProcessWelcome(std::vector<uint8_t>(payload.begin() + 2, payload.end()),
                                               recognized_with_self());
            if (roster) {
                prepare_ratchets(transition, mls_->GetProtocolVersion());
                ready_for(transition);
            } else {
                send_json({{"op", 31}, {"d", {{"transition_id", transition}}}});
                send_key_package();
            }
            break;
        }
        default:
            break;
        }
    }

    // ---- DAVE ---------------------------------------------------------------

    std::set<std::string> recognized_with_self() const {
        std::set<std::string> users = recognized_;
        users.insert(info_.user_id);
        return users;
    }

    void dave_init(int version) {
        if (version > 0) {
            init_group(version);
            send_key_package();
        } else {
            prepare_ratchets(0, version);
            execute_transition(0);
        }
    }

    void init_group(int version) {
        if (!mls_) {
            mls_ = dave::mls::CreateSession(nullptr, "", [](std::string const& source, std::string const& reason) {
                std::printf("[voice] MLS failure %s: %s\n", source.c_str(), reason.c_str());
            });
        }
        std::shared_ptr<::mlspp::SignaturePrivateKey> key;
        mls_->Init(dave::ProtocolVersion(version), std::stoull(info_.channel_id), info_.user_id, key);
    }

    void send_key_package() {
        if (mls_) {
            send_binary(26, mls_->GetMarshalledKeyPackage());
        }
    }

    void setup_ratchet(const std::string& user, int version) {
        auto& decryptor = decryptors_[user];
        if (!decryptor) {
            decryptor = dave::CreateDecryptor();
        }
        if (version == 0 || !mls_) {
            decryptor->TransitionToPassthroughMode(true);
        } else {
            decryptor->TransitionToKeyRatchet(mls_->GetKeyRatchet(user));
        }
    }

    void prepare_ratchets(uint16_t transition, int version) {
        for (const std::string& user : recognized_) {
            if (user != info_.user_id) {
                setup_ratchet(user, version);
            }
        }
        if (transition == 0) {
            setup_own_ratchet(version);
        } else {
            transitions_[transition] = version;
        }
        latest_version_ = version;
        owner_.set(Phase::connected, version > 0 ? i18n::T("Ses bağlantısı kuruldu, uçtan uca şifreli") : i18n::T("Ses bağlantısı kuruldu"),
                   version > 0);
    }

    void execute_transition(uint16_t transition) {
        auto it = transitions_.find(transition);
        if (it == transitions_.end()) {
            return;
        }
        const int version = it->second;
        transitions_.erase(it);
        if (version == 0 && mls_) {
            mls_->Reset();
        }
        setup_own_ratchet(version);
    }

    void setup_own_ratchet(int version) {
        if (version == 0 || !mls_) {
            encryptor_->SetPassthroughMode(true);
        } else {
            encryptor_->SetKeyRatchet(mls_->GetKeyRatchet(info_.user_id));
            encryptor_->SetPassthroughMode(false);
        }
    }

    void ready_for(uint16_t transition) {
        if (transition != 0) {
            send_json({{"op", 23}, {"d", {{"transition_id", transition}}}});
        }
    }

    // ---- UDP ----------------------------------------------------------------

    void on_ready(const json& d) {
        ssrc_ = d.value("ssrc", 0u);
        encryptor_->AssignSsrcToCodec(ssrc_, dave::Codec::Opus);
        const std::string ip = d.value("ip", std::string());
        const int port = d.value("port", 0);
        std::vector<std::string> modes = d.value("modes", std::vector<std::string>());

        udp_ = socket(AF_INET, SOCK_DGRAM, 0);
        if (udp_ < 0) {
            owner_.set(Phase::failed, i18n::T("Ses için UDP soketi açılamadı"));
            return;
        }
        server_.sin_family = AF_INET;
        server_.sin_port = htons(uint16_t(port));
        if (inet_pton(AF_INET, ip.c_str(), &server_.sin_addr) != 1 && !dc::net::resolve(ip, server_.sin_addr)) {
            owner_.set(Phase::failed, i18n::T("Ses sunucusunun adresi çözülemedi"));
            return;
        }
        timeval tv{1, 0};
        setsockopt(udp_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        std::string address;
        int external_port = 0;
        for (int attempt = 0; attempt < 3 && address.empty(); ++attempt) {
            uint8_t request[74] = {};
            put16(request, 1);
            put16(request + 2, 70);
            put32(request + 4, ssrc_);
            sendto(udp_, request, sizeof(request), 0, reinterpret_cast<sockaddr*>(&server_), sizeof(server_));
            uint8_t reply[74];
            const long n = recv(udp_, reply, sizeof(reply), 0);
            if (n == 74 && be16(reply) == 2) {
                address.assign(reinterpret_cast<const char*>(reply + 8), strnlen(reinterpret_cast<const char*>(reply + 8), 64));
                external_port = be16(reply + 72);
            }
        }
        if (address.empty()) {
            owner_.set(Phase::failed, i18n::T("Ses sunucusuna UDP ile ulaşılamadı"));
            return;
        }
        aes_ = std::find(modes.begin(), modes.end(), "aead_aes256_gcm_rtpsize") != modes.end() &&
               crypto_aead_aes256gcm_is_available();
        send_json({{"op", 1},
                   {"d",
                    {{"protocol", "udp"},
                     {"data",
                      {{"address", address},
                       {"port", external_port},
                       {"mode", aes_ ? "aead_aes256_gcm_rtpsize" : "aead_xchacha20_poly1305_rtpsize"}}}}}});
        next_keepalive_ = Clock::now() + std::chrono::seconds(5);
    }

    void on_session(const json& d) {
        const std::vector<uint8_t> key = d.value("secret_key", std::vector<uint8_t>());
        if (key.size() != key_.size()) {
            owner_.set(Phase::failed, i18n::T("Ses şifreleme anahtarı alınamadı"));
            return;
        }
        std::copy(key.begin(), key.end(), key_.begin());
        aes_ = d.value("mode", std::string()) == "aead_aes256_gcm_rtpsize";
        have_key_ = true;
        const int version = d.value("dave_protocol_version", 0);
        owner_.set(Phase::connected, version > 0 ? i18n::T("Ses bağlandı, şifreleme anahtarları bekleniyor")
                                                 : i18n::T("Ses bağlantısı kuruldu"));
        dave_init(version);
        // Sending once opens the media path through NAT and the SFU.
        send_json({{"op", 5}, {"d", {{"speaking", 1}, {"delay", 0}, {"ssrc", ssrc_}}}});
        for (int i = 0; i < 5; ++i) {
            send_opus(dave::kOpusSilencePacket.data(), dave::kOpusSilencePacket.size());
        }
        send_json({{"op", 5}, {"d", {{"speaking", 0}, {"delay", 0}, {"ssrc", ssrc_}}}});

        std::string error;
        if (mic_.open(error)) {
            int status = 0;
            encoder_ = opus_encoder_create(mic_.rate(), 1, OPUS_APPLICATION_VOIP, &status);
            if (encoder_) {
                opus_encoder_ctl(encoder_, OPUS_SET_BITRATE(64000));
            } else {
                mic_.close();
                error = i18n::T("Ses kodlayıcı açılamadı");
            }
        }
        owner_.set_mic(encoder_ != nullptr, error);
    }

    // Sends captured speech; a short silence and speaking 0 when it ends.
    void send_mic() {
        if (!encoder_) {
            return;
        }
        std::vector<int16_t> frame;
        while (mic_.pop(frame)) {
            double energy = 0;
            for (const int16_t sample : frame) {
                energy += double(sample) * sample;
            }
            const float level = float(std::sqrt(energy / double(frame.size())) / 32768.0);
            ++stats_mic_;
            stats_peak_ = std::max(stats_peak_, level);
            const bool muted = owner_.muted.load();
            if (!muted && level > kVoiceLevel) {
                hang_ = kHangFrames;
            } else if (hang_ > 0) {
                --hang_;
            }
            if (!muted && hang_ > 0) {
                if (!speaking_) {
                    send_json({{"op", 5}, {"d", {{"speaking", 1}, {"delay", 0}, {"ssrc", ssrc_}}}});
                    speaking_ = true;
                }
                uint8_t opus[1275];
                const int len = opus_encode(encoder_, frame.data(), int(frame.size()), opus, sizeof(opus));
                if (len > 0) {
                    ++stats_sent_;
                    send_encrypted(opus, size_t(len));
                }
            } else if (speaking_) {
                for (int i = 0; i < 5; ++i) {
                    send_opus(dave::kOpusSilencePacket.data(), dave::kOpusSilencePacket.size());
                }
                send_json({{"op", 5}, {"d", {{"speaking", 0}, {"delay", 0}, {"ssrc", ssrc_}}}});
                speaking_ = false;
                hang_ = 0;
            }
        }
    }

    void send_encrypted(const uint8_t* opus, size_t len) {
        sealed_.resize(encryptor_->GetMaxCiphertextByteSize(dave::MediaType::Audio, len));
        size_t written = 0;
        if (encryptor_->Encrypt(dave::MediaType::Audio, ssrc_, dave::MakeArrayView(opus, len),
                                dave::MakeArrayView(sealed_.data(), sealed_.size()),
                                &written) == dave::IEncryptor::Success) {
            send_opus(sealed_.data(), written);
        }
    }

    bool seal(uint8_t* out, unsigned long long* out_len, const uint8_t* plain, size_t plain_len, const uint8_t* ad,
              size_t ad_len, const uint8_t* nonce) const {
        return aes_ ? crypto_aead_aes256gcm_encrypt(out, out_len, plain, plain_len, ad, ad_len, nullptr, nonce,
                                                    key_.data()) == 0
                    : crypto_aead_xchacha20poly1305_ietf_encrypt(out, out_len, plain, plain_len, ad, ad_len, nullptr,
                                                                 nonce, key_.data()) == 0;
    }

    bool open(uint8_t* out, unsigned long long* out_len, const uint8_t* cipher, size_t cipher_len, const uint8_t* ad,
              size_t ad_len, const uint8_t* nonce) const {
        return aes_ ? crypto_aead_aes256gcm_decrypt(out, out_len, nullptr, cipher, cipher_len, ad, ad_len, nonce,
                                                    key_.data()) == 0
                    : crypto_aead_xchacha20poly1305_ietf_decrypt(out, out_len, nullptr, cipher, cipher_len, ad, ad_len,
                                                                 nonce, key_.data()) == 0;
    }

    void send_opus(const uint8_t* opus, size_t len) {
        std::vector<uint8_t> packet(12 + len + 16 + 4);
        packet[0] = 0x80;
        packet[1] = 0x78;
        put16(&packet[2], rtp_seq_++);
        put32(&packet[4], rtp_timestamp_);
        rtp_timestamp_ += kFrame;
        put32(&packet[8], ssrc_);
        uint8_t nonce[24] = {};
        put32(nonce, send_nonce_);
        unsigned long long sealed = 0;
        if (!seal(&packet[12], &sealed, opus, len, packet.data(), 12, nonce)) {
            return;
        }
        put32(&packet[12 + sealed], send_nonce_++);
        sendto(udp_, packet.data(), 12 + sealed + 4, 0, reinterpret_cast<sockaddr*>(&server_), sizeof(server_));
    }

    void keepalive() {
        if (udp_ < 0 || Clock::now() < next_keepalive_) {
            return;
        }
        uint8_t ping[8] = {};
        put32(ping, keepalive_count_++);
        sendto(udp_, ping, sizeof(ping), 0, reinterpret_cast<sockaddr*>(&server_), sizeof(server_));
        next_keepalive_ = Clock::now() + std::chrono::seconds(5);
    }

    void pump_udp() {
        if (udp_ < 0 || !have_key_) {
            return;
        }
        uint8_t packet[2048];
        for (int i = 0; i < 64; ++i) {
            const long n = recv(udp_, packet, sizeof(packet), MSG_DONTWAIT);
            if (n <= 0) {
                return;
            }
            ++stats_rx_;
            on_packet(packet, size_t(n));
        }
    }

    void on_packet(const uint8_t* packet, size_t n) {
        if (n < 12 + 4 + 16 || (packet[0] & 0xC0) != 0x80 || (packet[1] & 0x7f) != 0x78) {
            return;
        }
        const uint32_t ssrc = be32(packet + 8);
        size_t header = 12 + 4 * size_t(packet[0] & 0x0f);
        const bool extension = packet[0] & 0x10;
        if (extension) {
            header += 4;
        }
        if (n < header + 4 + 16) {
            return;
        }
        uint8_t nonce[24] = {};
        std::memcpy(nonce, packet + n - 4, 4);
        plain_.resize(n);
        unsigned long long plain_len = 0;
        if (!open(plain_.data(), &plain_len, packet + header, n - header - 4, packet, header, nonce)) {
            return;
        }
        size_t skip = extension ? 4 * size_t(be16(packet + header - 2)) : 0;
        if (skip > plain_len) {
            return;
        }
        const uint8_t* frame = plain_.data() + skip;
        const size_t frame_len = size_t(plain_len) - skip;

        const uint8_t* opus = frame;
        size_t opus_len = frame_len;
        const bool protocol_frame = frame_len >= 2 && frame[frame_len - 1] == 0xFA && frame[frame_len - 2] == 0xFA;
        if (protocol_frame) {
            auto user = ssrc_users_.find(ssrc);
            if (user == ssrc_users_.end()) {
                return;
            }
            auto decryptor = decryptors_.find(user->second);
            if (decryptor == decryptors_.end()) {
                return;
            }
            clear_.resize(decryptor->second->GetMaxPlaintextByteSize(dave::MediaType::Audio, frame_len));
            size_t written = 0;
            if (decryptor->second->Decrypt(dave::MediaType::Audio, dave::MakeArrayView(frame, frame_len),
                                           dave::MakeArrayView(clear_.data(), clear_.size()),
                                           &written) != dave::IDecryptor::Success) {
                return;
            }
            opus = clear_.data();
            opus_len = written;
        }
        decode(ssrc, opus, opus_len);
    }

    // ---- audio --------------------------------------------------------------

    struct Source {
        OpusDecoder* decoder = nullptr;
        std::deque<float> samples; // interleaved stereo
        Clock::time_point heard{};
    };

    void decode(uint32_t ssrc, const uint8_t* opus, size_t len) {
        Source& source = sources_[ssrc];
        if (!source.decoder) {
            int error = 0;
            source.decoder = opus_decoder_create(48000, 2, &error);
            if (!source.decoder) {
                return;
            }
        }
        pcm_.resize(5760 * 2);
        const int frames = opus_decode_float(source.decoder, opus, int(len), pcm_.data(), 5760, 0);
        if (frames <= 0) {
            return;
        }
        ++stats_decoded_;
        source.samples.insert(source.samples.end(), pcm_.begin(), pcm_.begin() + frames * 2);
        if (source.samples.size() > kMaxQueued) {
            source.samples.erase(source.samples.begin(),
                                 source.samples.begin() + long(source.samples.size() - kMaxQueued));
        }
        const bool silence = len == dave::kOpusSilencePacket.size() &&
                             std::equal(opus, opus + len, dave::kOpusSilencePacket.begin());
        if (!silence) {
            source.heard = Clock::now();
        }
    }

    void mix() {
        hui::audio::StreamRing* ring = owner_.ring;
        if (!ring) {
            return;
        }
        while (ring->available() < kTargetBuffered) {
            std::array<float, kFrame * 2> out{};
            bool any = false;
            for (auto& [ssrc, source] : sources_) {
                const size_t take = std::min(out.size(), source.samples.size());
                for (size_t i = 0; i < take; ++i) {
                    out[i] += source.samples[i];
                }
                source.samples.erase(source.samples.begin(), source.samples.begin() + long(take));
                any = any || take > 0;
            }
            if (!any) {
                return;
            }
            for (float& sample : out) {
                sample = std::clamp(sample, -1.0f, 1.0f);
                stats_out_ = std::max(stats_out_, std::fabs(sample));
            }
            ring->write(out.data(), kFrame);
            ++stats_mixed_;
        }
    }

    // Shows what keeps flowing when the app leaves the foreground.
    void log_stats() {
        if (Clock::now() < next_stats_) {
            return;
        }
        next_stats_ = Clock::now() + std::chrono::seconds(3);
        uint8_t status[256] = {};
        const int status_result = sceSystemServiceGetStatus ? sceSystemServiceGetStatus(status) : -1;
        size_t queued = 0;
        for (const auto& [ssrc, source] : sources_) {
            queued += source.samples.size() / 2;
        }
        std::printf("[voice] stats rx=%d decoded=%d mixed=%d queued=%zu ring=%zu mic=%d peak=%.3f out=%.3f silent=0x%x sent=%d "
                    "status=0x%x %02x %02x %02x %02x %02x %02x %02x %02x\n",
                    stats_rx_, stats_decoded_, stats_mixed_, queued, owner_.ring ? owner_.ring->available() : 0,
                    stats_mic_, stats_peak_, stats_out_, unsigned(mic_.silent_state()), stats_sent_, unsigned(status_result),
                    status[0], status[1], status[2], status[3], status[4], status[5], status[6], status[7]);
        stats_rx_ = stats_decoded_ = stats_mixed_ = stats_mic_ = stats_sent_ = 0;
        stats_peak_ = 0;
        stats_out_ = 0;
    }

    void report_speaking() {
        if (Clock::now() < next_report_) {
            return;
        }
        next_report_ = Clock::now() + std::chrono::milliseconds(150);
        std::set<std::string> speaking;
        const auto recent = Clock::now() - std::chrono::milliseconds(400);
        for (const auto& [ssrc, source] : sources_) {
            auto user = ssrc_users_.find(ssrc);
            if (source.heard > recent && user != ssrc_users_.end()) {
                speaking.insert(user->second);
            }
        }
        if (speaking_) {
            speaking.insert(info_.user_id);
        }
        owner_.set_speaking(std::move(speaking));
    }

    Engine::Impl& owner_;
    JoinInfo info_;
    dc::net::WebSocket ws_;
    int seq_ = -1;
    int beat_ms_ = 0;
    Clock::time_point next_beat_{};

    int udp_ = -1;
    sockaddr_in server_{};
    uint32_t ssrc_ = 0;
    bool aes_ = false;
    bool have_key_ = false;
    std::array<uint8_t, 32> key_{};
    uint32_t send_nonce_ = 0;
    uint16_t rtp_seq_ = 0;
    uint32_t rtp_timestamp_ = 0;
    uint32_t keepalive_count_ = 0;
    Clock::time_point next_keepalive_ = Clock::time_point::max();
    Clock::time_point next_report_{};
    Clock::time_point next_stats_{};
    int stats_rx_ = 0;
    int stats_decoded_ = 0;
    int stats_mixed_ = 0;
    int stats_mic_ = 0;
    int stats_sent_ = 0;
    float stats_peak_ = 0;
    float stats_out_ = 0;

    Mic mic_;
    OpusEncoder* encoder_ = nullptr;
    std::unique_ptr<dave::IEncryptor> encryptor_ = dave::CreateEncryptor();
    bool speaking_ = false;
    int hang_ = 0;
    std::vector<uint8_t> sealed_;

    std::unique_ptr<dave::mls::ISession> mls_;
    std::set<std::string> recognized_;
    std::map<std::string, std::unique_ptr<dave::IDecryptor>> decryptors_;
    std::map<uint16_t, int> transitions_;
    int latest_version_ = 0;

    std::map<uint32_t, std::string> ssrc_users_;
    std::map<uint32_t, Source> sources_;
    std::vector<uint8_t> plain_;
    std::vector<uint8_t> clear_;
    std::vector<float> pcm_;
};

void* run_connection(void* arg) {
    auto* impl = static_cast<Engine::Impl*>(arg);
    try {
        Connection connection(*impl, impl->info);
        connection.run();
    } catch (const std::exception& error) {
        impl->set(Phase::failed, i18n::T("Ses hatası: ") + std::string(error.what()));
    }
    return nullptr;
}

} // namespace

Engine& Engine::instance() {
    static Engine engine;
    return engine;
}

Engine::Engine() : impl_(new Impl) {
    if (sodium_init() < 0) {
        std::printf("[voice] libsodium init failed\n");
    }
}

Engine::~Engine() = default;

void Engine::set_output(hui::audio::StreamRing* ring) {
    impl_->ring = ring;
}

void Engine::start(const JoinInfo& info) {
    stop();
    impl_->stop = false;
    impl_->info = info;
    impl_->set(Phase::connecting, i18n::T("Ses kanalına bağlanılıyor..."));
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 1 << 20); // MLS and TLS work runs deep
    impl_->running = pthread_create(&impl_->thread, &attr, run_connection, impl_.get()) == 0;
    pthread_attr_destroy(&attr);
    if (!impl_->running) {
        impl_->set(Phase::failed, i18n::T("Ses iş parçacığı başlatılamadı"));
    }
}

void Engine::stop() {
    impl_->stop = true;
    if (impl_->running) {
        pthread_join(impl_->thread, nullptr);
        impl_->running = false;
    }
    impl_->set(Phase::idle, std::string());
    impl_->set_speaking({});
    impl_->set_mic(false, std::string());
}

void Engine::set_muted(bool muted) {
    impl_->muted = muted;
    std::lock_guard<std::mutex> hold(impl_->lock);
    impl_->status.muted = muted;
}

Status Engine::status() const {
    std::lock_guard<std::mutex> hold(impl_->lock);
    return impl_->status;
}

} // namespace voice
