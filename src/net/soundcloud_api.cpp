#include "sctui/net/soundcloud_api.hpp"
#include "sctui/core/logger.hpp"
#include <regex>
#include <iostream>
#include <thread>
#include <chrono>
#include <cctype>
#include <unordered_set>
#include <nlohmann/json.hpp>

namespace sctui::net {

namespace {
    // Verified fallback SoundCloud Web client_id
    constexpr const char* FallbackClientId = "iZIs9mchVcX5lhVR1EzGCcyUbdafLnoP";
    constexpr const char* ApiBase = "https://api-v2.soundcloud.com";

    int http_status(const core::Result<HttpResponse>& response) {
        return response ? response->status_code : response.error().http_status;
    }

    models::Track parse_track_object(const nlohmann::json& track_obj) {
        models::Track track;
        track.id = track_obj["id"].get<uint64_t>();

        if (track_obj.contains("title") && track_obj["title"].is_string()) {
            track.title = track_obj["title"].get<std::string>();
        }
        if (track_obj.contains("duration") && track_obj["duration"].is_number()) {
            track.duration_ms = track_obj["duration"].get<uint64_t>();
        }
        if (track_obj.contains("permalink_url") && track_obj["permalink_url"].is_string()) {
            track.permalink_url = track_obj["permalink_url"].get<std::string>();
        }
        if (track_obj.contains("artwork_url") && track_obj["artwork_url"].is_string()) {
            track.artwork_url = track_obj["artwork_url"].get<std::string>();
        }
        if (track_obj.contains("genre") && track_obj["genre"].is_string()) {
            track.genre = track_obj["genre"].get<std::string>();
        }
        if (track_obj.contains("description") && track_obj["description"].is_string()) {
            track.description = track_obj["description"].get<std::string>();
        }
        if (track_obj.contains("playback_count") && track_obj["playback_count"].is_number()) {
            track.playback_count = track_obj["playback_count"].get<uint64_t>();
        }
        if (track_obj.contains("likes_count") && track_obj["likes_count"].is_number()) {
            track.likes_count = track_obj["likes_count"].get<uint64_t>();
        }
        if (track_obj.contains("waveform_url") && track_obj["waveform_url"].is_string()) {
            track.waveform_url = track_obj["waveform_url"].get<std::string>();
        }
        if (track_obj.contains("policy") && track_obj["policy"].is_string()) {
            track.policy = track_obj["policy"].get<std::string>();
        }
        if (track_obj.contains("monetization_model") && track_obj["monetization_model"].is_string()) {
            track.monetization_model = track_obj["monetization_model"].get<std::string>();
        }
        if (track_obj.contains("user_favorite") && track_obj["user_favorite"].is_boolean()) {
            track.is_liked = track_obj["user_favorite"].get<bool>();
        }

        // User / Artist
        if (track_obj.contains("user") && track_obj["user"].is_object()) {
            const auto& user = track_obj["user"];
            if (user.contains("username") && user["username"].is_string()) {
                track.artist = user["username"].get<std::string>();
            }
            if (user.contains("id") && user["id"].is_number()) {
                track.artist_id = user["id"].get<uint64_t>();
            }
        }

        // Media & Transcodings
        bool has_unencrypted_full = false;
        bool has_encrypted_full = false;
        bool all_snipped = true;

        if (track_obj.contains("media") && track_obj["media"].is_object()) {
            const auto& media = track_obj["media"];
            if (media.contains("transcodings") && media["transcodings"].is_array()) {
                for (const auto& tc : media["transcodings"]) {
                    models::Transcoding transcoding;
                    if (tc.contains("url") && tc["url"].is_string()) {
                        transcoding.url = tc["url"].get<std::string>();
                    }
                    if (tc.contains("preset") && tc["preset"].is_string()) {
                        transcoding.preset = tc["preset"].get<std::string>();
                    }
                    if (tc.contains("format") && tc["format"].is_object()) {
                        const auto& fmt = tc["format"];
                        if (fmt.contains("protocol") && fmt["protocol"].is_string()) {
                            transcoding.protocol = fmt["protocol"].get<std::string>();
                        }
                        if (fmt.contains("mime_type") && fmt["mime_type"].is_string()) {
                            transcoding.mime_type = fmt["mime_type"].get<std::string>();
                        }
                    }
                    if (tc.contains("snipped") && tc["snipped"].is_boolean()) {
                        transcoding.snipped = tc["snipped"].get<bool>();
                    }

                    bool is_enc = (transcoding.protocol.find("encrypted") != std::string::npos ||
                                   transcoding.protocol == "cbc-encrypted-hls" ||
                                   transcoding.protocol == "ctr-encrypted-hls");
                    bool is_snip = transcoding.snipped || (transcoding.url.find("/preview/") != std::string::npos);

                    if (is_snip) {
                        // Snippet preview transcoding
                    } else if (is_enc) {
                        has_encrypted_full = true;
                        all_snipped = false;
                    } else {
                        has_unencrypted_full = true;
                        all_snipped = false;
                    }

                    track.transcodings.push_back(std::move(transcoding));
                }
            }
        }

        if (track.transcodings.empty()) {
            all_snipped = false;
        }

        // Categorize access: DRM-protected vs SoundCloud Go+ vs Playable
        // 1. Explicit Go+ tier from SoundCloud API: monetization SUB_HIGH_TIER, policy SNIP, or all transcodings snipped with NO full stream
        if (track.monetization_model == "SUB_HIGH_TIER" || track.policy == "SNIP" || (all_snipped && !track.transcodings.empty() && !has_encrypted_full && !has_unencrypted_full)) {
            track.access_type = models::TrackAccessType::GoPlus;
            track.is_snipped = true;
        } else if (has_encrypted_full && !has_unencrypted_full) {
            // Full song stream is available via Widevine CTR/CBC DRM encryption (e.g. Vxid, ad-supported free listening)
            track.access_type = models::TrackAccessType::DrmProtected;
            track.is_snipped = false;
        } else if (track.policy == "BLOCK") {
            // Region blocked or copyright blocked
            track.access_type = models::TrackAccessType::DrmProtected;
            track.is_snipped = false;
        } else {
            // Standard playable full stream
            track.access_type = models::TrackAccessType::Playable;
            track.is_snipped = false;
        }

        return track;
    }
}

SoundCloudApi::SoundCloudApi(std::string client_id, std::string auth_token)
    : client_id_(std::move(client_id)), auth_token_(std::move(auth_token)) {
    if (!auth_token_.empty()) {
        http_.set_bearer_token(auth_token_);
    }
}

SoundCloudApi::~SoundCloudApi() = default;

void SoundCloudApi::set_client_id(std::string client_id) {
    client_id_ = std::move(client_id);
}

const std::string& SoundCloudApi::get_client_id() const noexcept {
    return client_id_;
}

void SoundCloudApi::set_auth_token(std::string token) {
    auth_token_ = std::move(token);
    user_id_ = 0;
    http_.set_bearer_token(auth_token_);
}

const std::string& SoundCloudApi::get_auth_token() const noexcept {
    return auth_token_;
}

void SoundCloudApi::configure_network(
    const std::string& doh_url,
    const std::string& dns_servers,
    const std::string& proxy
) {
    http_.set_doh_url(doh_url);
    http_.set_dns_servers(dns_servers);
    http_.set_proxy(proxy);
}

core::Result<std::string> SoundCloudApi::resolve_client_id() {
    if (!client_id_.empty()) {
        return client_id_;
    }

    // Attempt to scrape dynamically from soundcloud.com
    auto home_res = http_.get("https://soundcloud.com");
    if (home_res && home_res->is_2xx()) {
        std::regex script_regex(R"regex(src="(https://a-v2\.sndcdn\.com/assets/[^"]+\.js)")regex");
        auto reg_begin = std::sregex_iterator(home_res->body.begin(), home_res->body.end(), script_regex);
        auto reg_end = std::sregex_iterator();

        std::vector<std::string> script_urls;
        for (auto it = reg_begin; it != reg_end; ++it) {
            script_urls.push_back((*it)[1].str());
        }

        // Search backward through scripts (assets bundle typically contains client_id)
        std::regex id_regex(R"regex(client_id\s*[:=]\s*["']([a-zA-Z0-9]{32})["'])regex");
        for (auto it = script_urls.rbegin(); it != script_urls.rend(); ++it) {
            auto script_res = http_.get(*it);
            if (script_res && script_res->is_2xx()) {
                std::smatch match;
                if (std::regex_search(script_res->body, match, id_regex)) {
                    client_id_ = match[1].str();
                    return client_id_;
                }
            }
        }
    }

    // Graceful fallback to verified client ID
    client_id_ = FallbackClientId;
    return client_id_;
}

core::Result<std::vector<models::Track>> SoundCloudApi::search_tracks(
    const std::string& query,
    size_t limit,
    size_t offset
) {
    if (client_id_.empty()) {
        auto res = resolve_client_id();
        if (!res) return core::MakeError(core::ErrorCode::AuthError, "Unable to resolve SoundCloud client ID");
    }

    std::string url = std::string(ApiBase) + "/search/tracks";
    std::unordered_map<std::string, std::string> params = {
        {"q", query},
        {"client_id", client_id_},
        {"limit", std::to_string(limit)},
        {"offset", std::to_string(offset)}
    };

    auto resp = http_.get(url, {}, params);
    if (!resp) {
        return core::MakeError(resp.error().code, resp.error().message, resp.error().http_status);
    }

    return parse_tracks_json(resp->body);
}

core::Result<std::vector<models::Playlist>> SoundCloudApi::search_playlists(
    const std::string& query,
    size_t limit,
    size_t offset
) {
    if (client_id_.empty()) {
        auto res = resolve_client_id();
        if (!res) return core::MakeError(core::ErrorCode::AuthError, "Unable to resolve SoundCloud client ID");
    }

    std::string url = std::string(ApiBase) + "/search/playlists";
    std::unordered_map<std::string, std::string> params = {
        {"q", query},
        {"client_id", client_id_},
        {"limit", std::to_string(limit)},
        {"offset", std::to_string(offset)}
    };

    auto resp = http_.get(url, {}, params);
    if (!resp) {
        return core::MakeError(resp.error().code, resp.error().message, resp.error().http_status);
    }

    return parse_playlists_json(resp->body);
}

core::Result<uint64_t> SoundCloudApi::resolve_user_id() {
    if (user_id_ > 0) {
        return user_id_;
    }
    if (auth_token_.empty()) {
        return core::MakeError(core::ErrorCode::AuthError, "OAuth token required to resolve user ID");
    }

    std::string url = std::string(ApiBase) + "/me";
    std::unordered_map<std::string, std::string> params;
    if (!client_id_.empty()) {
        params["client_id"] = client_id_;
    }

    auto resp = http_.get(url, {}, params);
    if (!resp || !resp->is_2xx()) {
        std::string err = resp ? "HTTP " + std::to_string(resp->status_code) : resp.error().message;
        LOG_ERROR("SoundCloudApi", "Failed to resolve user info via /me: " + err);
        return core::MakeError(core::ErrorCode::AuthError, "Failed to authenticate with token: " + err);
    }

    try {
        auto j = nlohmann::json::parse(resp->body);
        if (j.contains("id") && j["id"].is_number()) {
            user_id_ = j["id"].get<uint64_t>();
            std::string username = j.value("username", "");
            LOG_INFO("SoundCloudApi", "Authenticated as user: " + username + " (ID: " + std::to_string(user_id_) + ")");
            return user_id_;
        }
    } catch (const std::exception& e) {
        LOG_ERROR("SoundCloudApi", "Failed to parse /me JSON response: " + std::string(e.what()));
        return core::MakeError(core::ErrorCode::JsonParseError, e.what());
    }

    return core::MakeError(core::ErrorCode::AuthError, "User ID not found in /me response");
}

core::Result<models::UserProfile> SoundCloudApi::get_me() {
    if (auth_token_.empty()) {
        return core::MakeError(core::ErrorCode::AuthError, "OAuth token required to load profile");
    }

    std::string url = std::string(ApiBase) + "/me";
    std::unordered_map<std::string, std::string> params;
    if (!client_id_.empty()) {
        params["client_id"] = client_id_;
    }

    auto resp = http_.get(url, {}, params);
    if (!resp || !resp->is_2xx()) {
        std::string err = resp ? "HTTP " + std::to_string(resp->status_code) : resp.error().message;
        LOG_ERROR("SoundCloudApi", "Failed to get user profile via /me: " + err);
        return core::MakeError(core::ErrorCode::AuthError, "Failed to load profile: " + err);
    }

    try {
        auto j = nlohmann::json::parse(resp->body);
        models::UserProfile profile;
        profile.id = j.value("id", 0ULL);
        profile.username = j.value("username", "");
        profile.permalink = j.value("permalink", "");
        profile.permalink_url = j.value("permalink_url", "");
        profile.avatar_url = j.value("avatar_url", "");
        if (j.contains("description") && j["description"].is_string()) {
            profile.description = j["description"].get<std::string>();
        }
        if (j.contains("country") && j["country"].is_string()) {
            profile.country = j["country"].get<std::string>();
        }
        if (j.contains("city") && j["city"].is_string()) {
            profile.city = j["city"].get<std::string>();
        }
        if (j.contains("plan") && j["plan"].is_string()) {
            profile.plan = j["plan"].get<std::string>();
        }
        if (j.contains("consumer_subscription") && j["consumer_subscription"].is_object()) {
            const auto& cs = j["consumer_subscription"];
            if (cs.contains("product") && cs["product"].is_object()) {
                std::string pid = cs["product"].value("id", "free");
                profile.consumer_subscription_id = pid;
                std::string lower_pid = pid;
                std::transform(lower_pid.begin(), lower_pid.end(), lower_pid.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                if (lower_pid.find("high") != std::string::npos || lower_pid.find("go-plus") != std::string::npos ||
                    lower_pid.find("go_plus") != std::string::npos || lower_pid.find("goplus") != std::string::npos) {
                    profile.subscription_tier = models::SubscriptionTier::GoPlus;
                } else if (lower_pid.find("mid") != std::string::npos || lower_pid.find("go") != std::string::npos) {
                    if (lower_pid != "free") {
                        profile.subscription_tier = models::SubscriptionTier::Go;
                    }
                }
            }
        }
        if (profile.subscription_tier == models::SubscriptionTier::Free && !profile.plan.empty()) {
            std::string lower_plan = profile.plan;
            std::transform(lower_plan.begin(), lower_plan.end(), lower_plan.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (lower_plan.find("go+") != std::string::npos || lower_plan.find("go plus") != std::string::npos || lower_plan.find("goplus") != std::string::npos) {
                profile.subscription_tier = models::SubscriptionTier::GoPlus;
            } else if (lower_plan.find("go") != std::string::npos) {
                profile.subscription_tier = models::SubscriptionTier::Go;
            }
        }
        if (j.contains("first_name") && j["first_name"].is_string()) {
            profile.first_name = j["first_name"].get<std::string>();
        }
        if (j.contains("last_name") && j["last_name"].is_string()) {
            profile.last_name = j["last_name"].get<std::string>();
        }
        if (j.contains("full_name") && j["full_name"].is_string()) {
            profile.full_name = j["full_name"].get<std::string>();
        }
        if (j.contains("primary_email") && j["primary_email"].is_string()) {
            profile.primary_email = j["primary_email"].get<std::string>();
        }
        if (j.contains("created_at") && j["created_at"].is_string()) {
            profile.created_at = j["created_at"].get<std::string>();
        }
        if (j.contains("verified") && j["verified"].is_boolean()) {
            profile.is_verified = j["verified"].get<bool>();
        }
        if (j.contains("badges") && j["badges"].is_object()) {
            const auto& b = j["badges"];
            profile.is_pro = b.value("pro", false) || b.value("pro_unlimited", false) || b.value("creator_mid_tier", false);
            if (b.value("verified", false)) profile.is_verified = true;
        }
        profile.followers_count = j.value("followers_count", 0ULL);
        profile.followings_count = j.value("followings_count", 0ULL);
        profile.track_count = j.value("track_count", 0ULL);
        profile.likes_count = j.value("likes_count", 0ULL);
        profile.playlist_count = j.value("playlist_count", 0ULL);
        profile.reposts_count = j.value("reposts_count", 0ULL);

        user_id_ = profile.id;
        return profile;
    } catch (const std::exception& e) {
        return core::MakeError(core::ErrorCode::JsonParseError, e.what());
    }
}

core::Result<std::vector<models::Track>> SoundCloudApi::get_user_uploaded_tracks(uint64_t user_id, size_t limit) {
    if (user_id == 0) {
        auto uid_res = resolve_user_id();
        if (!uid_res) return core::MakeError(uid_res.error().code, uid_res.error().message);
        user_id = *uid_res;
    }

    std::string url = std::string(ApiBase) + "/users/" + std::to_string(user_id) + "/tracks";
    std::unordered_map<std::string, std::string> params;
    if (!client_id_.empty()) {
        params["client_id"] = client_id_;
    }
    params["limit"] = std::to_string(limit);

    auto resp = http_.get(url, {}, params);
    if (!resp || !resp->is_2xx()) {
        std::string err = resp ? "HTTP " + std::to_string(resp->status_code) : resp.error().message;
        LOG_ERROR("SoundCloudApi", "Failed to get user tracks: " + err);
        return core::MakeError(core::ErrorCode::HttpError, "Failed to get user tracks: " + err);
    }

    return parse_tracks_json(resp->body);
}

core::Result<SoundCloudApi::LikesPage> SoundCloudApi::get_user_likes_page(
    size_t limit,
    const std::string& next_url
) {
    if (auth_token_.empty()) {
        LOG_WARN("SoundCloudApi", "get_user_likes_page requested without auth_token");
        return core::MakeError(core::ErrorCode::AuthError, "No auth_token configured. Add your SoundCloud oauth_token to ~/.config/sctui/config.json to load your likes.");
    }

    if (client_id_.empty()) {
        auto cid_res = resolve_client_id();
        if (!cid_res) {
            LOG_WARN("SoundCloudApi", "Unable to resolve SoundCloud client ID for likes");
        }
    }

    std::string fetch_url;
    std::unordered_map<std::string, std::string> params;

    if (!next_url.empty()) {
        fetch_url = next_url;
        if (fetch_url.find("client_id") == std::string::npos && !client_id_.empty()) {
            params["client_id"] = client_id_;
        }
    } else {
        auto uid_res = resolve_user_id();
        if (!uid_res) {
            return core::MakeError(uid_res.error().code, uid_res.error().message);
        }
        fetch_url = std::string(ApiBase) + "/users/" + std::to_string(*uid_res) + "/track_likes";
        params["limit"] = std::to_string(limit);
        if (!client_id_.empty()) {
            params["client_id"] = client_id_;
        }
    }

    core::Result<HttpResponse> resp;
    for (int attempt = 0; attempt < 4; ++attempt) {
        resp = http_.get(fetch_url, {}, params);
        if (resp && resp->is_2xx()) {
            break;
        }
        if (http_status(resp) == 401 || http_status(resp) == 403) {
            (void)resolve_client_id();
            if (!client_id_.empty()) {
                params["client_id"] = client_id_;
            }
        }
        int delay = (http_status(resp) == 429) ? (1200 * (attempt + 1)) : (350 * (attempt + 1));
        std::this_thread::sleep_for(std::chrono::milliseconds(delay));
    }

    if (!resp || !resp->is_2xx()) {
        std::string err = resp ? "HTTP " + std::to_string(resp->status_code) : resp.error().message;
        LOG_ERROR("SoundCloudApi", "Failed to fetch user likes page after retries: " + err);
        return core::MakeError(core::ErrorCode::NetworkError, err);
    }

    try {
        auto j = nlohmann::json::parse(resp->body);
        LikesPage page;
        if (j.contains("collection") && j["collection"].is_array()) {
            for (const auto& item : j["collection"]) {
                const nlohmann::json* track_obj = &item;
                if (item.contains("track") && item["track"].is_object()) {
                    track_obj = &item["track"];
                }
                if (track_obj->contains("id") && (*track_obj)["id"].is_number()) {
                    auto trk = parse_track_object(*track_obj);
                    trk.is_liked = true;
                    page.tracks.push_back(std::move(trk));
                }
            }
        }
        if (j.contains("next_href") && j["next_href"].is_string()) {
            page.next_href = j["next_href"].get<std::string>();
        }
        return page;
    } catch (const std::exception& e) {
        LOG_ERROR("SoundCloudApi", "Failed to parse likes page JSON: " + std::string(e.what()));
        return core::MakeError(core::ErrorCode::JsonParseError, e.what());
    }
}

core::Result<std::vector<models::Track>> SoundCloudApi::get_user_likes(
    size_t limit,
    size_t /*offset*/
) {
    auto page_res = get_user_likes_page(limit);
    if (!page_res) {
        return core::MakeError(page_res.error().code, page_res.error().message);
    }
    return std::move(page_res->tracks);
}

core::Result<std::vector<models::Track>> SoundCloudApi::get_user_stream(
    size_t limit,
    size_t offset
) {
    if (auth_token_.empty()) {
        LOG_WARN("SoundCloudApi", "get_user_stream requested without auth_token. Prompting user to add oauth_token in ~/.config/sctui/config.json");
        return core::MakeError(core::ErrorCode::AuthError, "No auth_token configured. Add your SoundCloud oauth_token to ~/.config/sctui/config.json to load your stream.");
    }

    std::string url = std::string(ApiBase) + "/stream";
    std::unordered_map<std::string, std::string> params = {
        {"limit", std::to_string(limit)},
        {"offset", std::to_string(offset)}
    };
    if (!client_id_.empty()) {
        params["client_id"] = client_id_;
    }

    auto resp = http_.get(url, {}, params);
    if (!resp) {
        LOG_ERROR("SoundCloudApi", "Failed to fetch user stream HTTP " + std::to_string(resp.error().http_status) + ": " + resp.error().message);
        return core::MakeError(resp.error().code, resp.error().message, resp.error().http_status);
    }

    return parse_tracks_json(resp->body);
}

core::Result<std::vector<models::Playlist>> SoundCloudApi::get_user_playlists(
    size_t limit,
    size_t offset
) {
    if (auth_token_.empty()) {
        LOG_WARN("SoundCloudApi", "get_user_playlists requested without auth_token");
        return core::MakeError(core::ErrorCode::AuthError, "No auth_token configured. Add your SoundCloud oauth_token to ~/.config/sctui/config.json to load your playlists.");
    }

    auto uid_res = resolve_user_id();
    if (!uid_res) {
        return core::MakeError(uid_res.error().code, uid_res.error().message);
    }

    if (client_id_.empty()) {
        (void)resolve_client_id();
    }

    std::unordered_map<std::string, std::string> params = {
        {"limit", std::to_string(limit)},
        {"offset", std::to_string(offset)}
    };
    if (!client_id_.empty()) {
        params["client_id"] = client_id_;
    }

    std::vector<models::Playlist> playlists;
    std::unordered_set<uint64_t> seen_ids;

    // 1. Fetch authored playlists
    std::string authored_url = std::string(ApiBase) + "/users/" + std::to_string(*uid_res) + "/playlists";
    auto resp = http_.get(authored_url, {}, params);
    if (resp && resp->is_2xx()) {
        auto parsed = parse_playlists_json(resp->body);
        if (parsed) {
            for (auto& pl : *parsed) {
                if (seen_ids.insert(pl.id).second) {
                    playlists.push_back(std::move(pl));
                }
            }
        }
    }

    // 2. Fetch liked/favorited playlists from library
    std::string likes_url = std::string(ApiBase) + "/users/" + std::to_string(*uid_res) + "/playlist_likes";
    auto likes_resp = http_.get(likes_url, {}, params);
    if (likes_resp && likes_resp->is_2xx()) {
        auto parsed_likes = parse_playlists_json(likes_resp->body);
        if (parsed_likes) {
            for (auto& pl : *parsed_likes) {
                if (seen_ids.insert(pl.id).second) {
                    playlists.push_back(std::move(pl));
                }
            }
        }
    }

    if (playlists.empty() && (!resp || !resp->is_2xx()) && (!likes_resp || !likes_resp->is_2xx())) {
        std::string err = resp ? "HTTP " + std::to_string(resp->status_code) : resp.error().message;
        LOG_ERROR("SoundCloudApi", "Failed to fetch playlists: " + err);
        return core::MakeError(core::ErrorCode::NetworkError, err);
    }

    LOG_INFO("SoundCloudApi", "Fetched " + std::to_string(playlists.size()) + " user playlists (authored + liked)");
    return playlists;
}

core::Result<std::vector<models::Track>> SoundCloudApi::get_playlist_tracks(uint64_t playlist_id) {
    if (client_id_.empty()) {
        auto res = resolve_client_id();
        if (!res) return core::MakeError(core::ErrorCode::AuthError, "Unable to resolve SoundCloud client ID");
    }

    std::string url = std::string(ApiBase) + "/playlists/" + std::to_string(playlist_id);
    std::unordered_map<std::string, std::string> params;
    if (!client_id_.empty()) {
        params["client_id"] = client_id_;
    }

    core::Result<HttpResponse> resp;
    for (int attempt = 0; attempt < 3; ++attempt) {
        resp = http_.get(url, {}, params);
        if (resp && resp->is_2xx()) {
            break;
        }
        if (http_status(resp) == 401 || http_status(resp) == 403) {
            (void)resolve_client_id();
            if (!client_id_.empty()) {
                params["client_id"] = client_id_;
            }
        }
        int delay = (http_status(resp) == 429) ? (1000 * (attempt + 1)) : (300 * (attempt + 1));
        std::this_thread::sleep_for(std::chrono::milliseconds(delay));
    }

    if (!resp || !resp->is_2xx()) {
        std::string err = resp ? "HTTP " + std::to_string(resp->status_code) : resp.error().message;
        LOG_ERROR("SoundCloudApi", "Failed to fetch playlist " + std::to_string(playlist_id) + " after retries: " + err);
        return core::MakeError(core::ErrorCode::NetworkError, err);
    }

    try {
        auto j = nlohmann::json::parse(resp->body);
        if (!j.contains("tracks") || !j["tracks"].is_array()) {
            return std::vector<models::Track>{};
        }

        std::vector<uint64_t> track_order;
        std::unordered_map<uint64_t, models::Track> resolved_map;
        std::vector<std::string> stub_ids;

        for (const auto& item : j["tracks"]) {
            if (!item.contains("id") || !item["id"].is_number()) continue;
            uint64_t tid = item["id"].get<uint64_t>();
            track_order.push_back(tid);

            if (item.contains("title") && item["title"].is_string() && !item["title"].get<std::string>().empty()) {
                resolved_map[tid] = parse_track_object(item);
            } else {
                stub_ids.push_back(std::to_string(tid));
            }
        }

        // Batch resolve stub IDs in chunks of 50
        for (size_t i = 0; i < stub_ids.size(); i += 50) {
            size_t batch_end = std::min(i + 50, stub_ids.size());
            std::string ids_str;
            for (size_t k = i; k < batch_end; ++k) {
                if (k > i) ids_str += ",";
                ids_str += stub_ids[k];
            }

            std::unordered_map<std::string, std::string> batch_params = {
                {"ids", ids_str},
                {"client_id", client_id_}
            };

            core::Result<HttpResponse> batch_resp;
            for (int attempt = 0; attempt < 4; ++attempt) {
                batch_resp = http_.get(std::string(ApiBase) + "/tracks", {}, batch_params);
                if (batch_resp && batch_resp->is_2xx()) {
                    break;
                }
                if (http_status(batch_resp) == 401 || http_status(batch_resp) == 403) {
                    (void)resolve_client_id();
                    if (!client_id_.empty()) {
                        batch_params["client_id"] = client_id_;
                    }
                }
                int delay = (http_status(batch_resp) == 429) ? (1500 * (attempt + 1)) : (400 * (attempt + 1));
                std::this_thread::sleep_for(std::chrono::milliseconds(delay));
            }

            if (batch_resp && batch_resp->is_2xx()) {
                auto batch_tracks = parse_tracks_json(batch_resp->body);
                if (batch_tracks) {
                    for (auto& bt : *batch_tracks) {
                        resolved_map[bt.id] = std::move(bt);
                    }
                }
            } else {
                LOG_WARN("SoundCloudApi", "Failed to resolve track stub batch (" + ids_str + ") after retries");
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(60));
        }

        // Reconstruct in exact playlist order
        std::vector<models::Track> final_tracks;
        final_tracks.reserve(track_order.size());
        for (uint64_t tid : track_order) {
            auto it = resolved_map.find(tid);
            if (it != resolved_map.end()) {
                final_tracks.push_back(it->second);
            }
        }

        if (final_tracks.size() < track_order.size()) {
            LOG_INFO("SoundCloudApi", "Resolved " + std::to_string(final_tracks.size()) + " of " +
                     std::to_string(track_order.size()) + " tracks for playlist " + std::to_string(playlist_id) +
                     " (" + std::to_string(track_order.size() - final_tracks.size()) + " tracks unavailable or removed)");
        } else {
            LOG_INFO("SoundCloudApi", "Resolved all " + std::to_string(final_tracks.size()) + " tracks for playlist " + std::to_string(playlist_id));
        }
        return final_tracks;
    } catch (const std::exception& e) {
        LOG_ERROR("SoundCloudApi", "Failed to parse playlist tracks JSON: " + std::string(e.what()));
        return core::MakeError(core::ErrorCode::JsonParseError, e.what());
    }
}

core::Result<models::Track> SoundCloudApi::get_track_details(uint64_t track_id) {
    if (client_id_.empty()) {
        auto res = resolve_client_id();
        if (!res) return core::MakeError(core::ErrorCode::AuthError, "Unable to resolve SoundCloud client ID");
    }

    std::string url = std::string(ApiBase) + "/tracks/" + std::to_string(track_id);
    std::unordered_map<std::string, std::string> params = {
        {"client_id", client_id_}
    };

    core::Result<HttpResponse> resp;
    for (int attempt = 0; attempt < 3; ++attempt) {
        resp = http_.get(url, {}, params);
        if (resp && resp->is_2xx()) break;
        if (http_status(resp) == 401 || http_status(resp) == 403) {
            (void)resolve_client_id();
            if (!client_id_.empty()) params["client_id"] = client_id_;
        }
        int delay = (http_status(resp) == 429) ? (1000 * (attempt + 1)) : (300 * (attempt + 1));
        std::this_thread::sleep_for(std::chrono::milliseconds(delay));
    }

    if (!resp || !resp->is_2xx()) {
        std::string err = resp ? "HTTP " + std::to_string(resp->status_code) : resp.error().message;
        LOG_ERROR("SoundCloudApi", "Failed to fetch track details for " + std::to_string(track_id) + ": " + err);
        return core::MakeError(core::ErrorCode::NetworkError, err);
    }

    try {
        auto j = nlohmann::json::parse(resp->body);
        return parse_track_object(j);
    } catch (const std::exception& e) {
        LOG_ERROR("SoundCloudApi", "Failed to parse track details JSON for " + std::to_string(track_id) + ": " + std::string(e.what()));
        return core::MakeError(core::ErrorCode::JsonParseError, e.what());
    }
}

core::Result<std::string> SoundCloudApi::resolve_stream_url(const models::Track& track, bool allow_drm) {
    if (client_id_.empty()) {
        auto res = resolve_client_id();
        if (!res) return core::MakeError(core::ErrorCode::AuthError, "Unable to resolve SoundCloud client ID");
    }

    if (!track.stream_url.empty()) {
        return track.stream_url;
    }

    models::Track resolved_track = track;
    if (resolved_track.transcodings.empty()) {
        LOG_INFO("SoundCloudApi", "Track " + std::to_string(track.id) + " ('" + track.title + "') has no transcodings, fetching full track details from API...");
        auto detail_res = get_track_details(track.id);
        if (detail_res && !detail_res->transcodings.empty()) {
            resolved_track = std::move(*detail_res);
            LOG_INFO("SoundCloudApi", "Resolved full track details for " + std::to_string(track.id) + " (" + std::to_string(resolved_track.transcodings.size()) + " transcodings)");
        } else {
            LOG_ERROR("SoundCloudApi", "Track " + std::to_string(track.id) + " ('" + track.title + "') has no available transcodings after details fetch");
            return core::MakeError(core::ErrorCode::ResourceNotFound, "Track has no available transcodings");
        }
    }

    if (resolved_track.is_drm_protected() && !allow_drm) {
        LOG_WARN("SoundCloudApi", "Track " + std::to_string(resolved_track.id) + " ('" + resolved_track.title + "') is DRM-protected");
        return core::MakeError(core::ErrorCode::AuthError, "[DRM Protected] Track is DRM-encrypted (CBC/CTR) and cannot be played");
    }

    std::string transcoding_url;

    // Helper lambda to check if a transcoding is a 30s preview snippet
    auto is_preview = [](const models::Transcoding& t) {
        return t.snipped || (t.url.find("/preview/") != std::string::npos);
    };

    // 1. Try full unencrypted progressive mp3
    for (const auto& t : resolved_track.transcodings) {
        if (!is_preview(t) && t.protocol == "progressive" && t.mime_type.find("mpeg") != std::string::npos) {
            transcoding_url = t.url;
            LOG_DEBUG("SoundCloudApi", "Selected full progressive MP3 transcoding: " + t.url);
            break;
        }
    }

    // 2. Try full unencrypted other progressive
    if (transcoding_url.empty()) {
        for (const auto& t : resolved_track.transcodings) {
            if (!is_preview(t) && t.protocol == "progressive") {
                transcoding_url = t.url;
                LOG_DEBUG("SoundCloudApi", "Selected full progressive transcoding: " + t.url);
                break;
            }
        }
    }

    // 3. Try full unencrypted HLS AAC (160k or 96k)
    if (transcoding_url.empty()) {
        for (const auto& t : resolved_track.transcodings) {
            if (!is_preview(t) && t.protocol == "hls" && (t.preset == "aac_160k" || t.preset == "aac_96k" || t.mime_type.find("mp4") != std::string::npos)) {
                transcoding_url = t.url;
                LOG_DEBUG("SoundCloudApi", "Selected full HLS AAC transcoding: " + t.url);
                break;
            }
        }
    }

    // 4. Try any full unencrypted HLS
    if (transcoding_url.empty()) {
        for (const auto& t : resolved_track.transcodings) {
            if (!is_preview(t) && t.protocol == "hls") {
                transcoding_url = t.url;
                LOG_DEBUG("SoundCloudApi", "Selected full unencrypted general HLS transcoding: " + t.url);
                break;
            }
        }
    }

    // 5. If DRM is enabled and no full unencrypted stream was chosen, select DRM-protected stream
    if (transcoding_url.empty() && allow_drm) {
        // Priority 1: Widevine CTR encrypted HLS (supported on Linux/macOS/Windows via CastLabs Widevine)
        for (const auto& t : resolved_track.transcodings) {
            if (!is_preview(t) && (t.protocol == "ctr-encrypted-hls" || t.url.find("ctr-encrypted-hls") != std::string::npos)) {
                transcoding_url = t.url;
                LOG_INFO("SoundCloudApi", "Selected Widevine CTR encrypted HLS transcoding: " + t.url + " (" + t.preset + ")");
                break;
            }
        }
        // Priority 2: CBC encrypted HLS
        if (transcoding_url.empty()) {
            for (const auto& t : resolved_track.transcodings) {
                if (!is_preview(t) && (t.protocol == "cbc-encrypted-hls" || t.url.find("cbc-encrypted-hls") != std::string::npos)) {
                    transcoding_url = t.url;
                    LOG_INFO("SoundCloudApi", "Selected CBC encrypted HLS transcoding: " + t.url + " (" + t.preset + ")");
                    break;
                }
            }
        }
        // Priority 3: any full encrypted stream
        if (transcoding_url.empty()) {
            for (const auto& t : resolved_track.transcodings) {
                if (!is_preview(t) && (t.protocol.find("encrypted") != std::string::npos || t.url.find("encrypted") != std::string::npos)) {
                    transcoding_url = t.url;
                    LOG_INFO("SoundCloudApi", "Selected general encrypted HLS transcoding: " + t.url);
                    break;
                }
            }
        }
    }

    // 6. Last resort fallback: preview snippet only if no full stream exists (Go+ tracks)
    if (transcoding_url.empty()) {
        for (const auto& t : resolved_track.transcodings) {
            if (t.protocol == "progressive") {
                transcoding_url = t.url;
                LOG_DEBUG("SoundCloudApi", "Selected preview progressive transcoding: " + t.url);
                break;
            }
        }
        if (transcoding_url.empty() && !resolved_track.transcodings.empty()) {
            transcoding_url = resolved_track.transcodings.front().url;
        }
    }

    std::unordered_map<std::string, std::string> params = {
        {"client_id", client_id_}
    };

    auto resp = http_.get(transcoding_url, {}, params);
    if (!resp || !resp->is_2xx()) {
        std::string err_msg;
        if (resp) {
            if (resp->status_code == 403 || resp->status_code == 401) {
                err_msg = "[SoundCloud Go+] Track requires SoundCloud Go+ subscription";
            } else if (resp->status_code == 404) {
                err_msg = "Stream URL not found on SoundCloud servers (404)";
            } else {
                err_msg = "HTTP " + std::to_string(resp->status_code) + " error resolving stream";
            }
        } else {
            err_msg = resp.error().message;
        }
        LOG_ERROR("SoundCloudApi", "Failed to resolve stream link for track " + std::to_string(track.id) + " ('" + track.title + "'): " + err_msg);
        return core::MakeError(core::ErrorCode::AuthError, err_msg);
    }

    try {
        auto j = nlohmann::json::parse(resp->body);
        if (j.contains("url") && j["url"].is_string()) {
            std::string resolved = j["url"].get<std::string>();
            if (j.contains("licenseAuthToken") && j["licenseAuthToken"].is_string()) {
                std::string token = j["licenseAuthToken"].get<std::string>();
                resolved += "#license_token=" + token;
                LOG_INFO("SoundCloudApi", "Resolved DRM audio stream with Widevine licenseAuthToken for track " + std::to_string(track.id));
            } else {
                LOG_INFO("SoundCloudApi", "Resolved audio stream URL for track " + std::to_string(track.id) + " ('" + track.title + "')");
            }
            return resolved;
        }
    } catch (const std::exception& e) {
        LOG_ERROR("SoundCloudApi", "Failed to parse stream URL JSON response for track " + std::to_string(track.id) + ": " + e.what());
        return core::MakeError(core::ErrorCode::JsonParseError, std::string("Failed to parse stream URL response: ") + e.what());
    }

    LOG_ERROR("SoundCloudApi", "Resolved stream payload did not contain URL for track " + std::to_string(track.id));
    return core::MakeError(core::ErrorCode::ResourceNotFound, "Resolved stream payload did not contain URL");
}

core::Result<std::vector<float>> SoundCloudApi::fetch_waveform_samples(const std::string& waveform_url) {
    if (waveform_url.empty()) {
        return std::vector<float>{};
    }

    // Normalize URL to JSON endpoint if png
    std::string json_url = waveform_url;
    auto png_pos = json_url.find(".png");
    if (png_pos != std::string::npos) {
        json_url.replace(png_pos, 4, ".json");
    }

    auto resp = http_.get(json_url);
    if (!resp || !resp->is_2xx()) {
        return std::vector<float>{};
    }

    try {
        auto j = nlohmann::json::parse(resp->body);
        if (j.contains("samples") && j["samples"].is_array()) {
            float max_val = 1.0f;
            if (j.contains("height") && j["height"].is_number()) {
                max_val = std::max(1.0f, j["height"].get<float>());
            }

            std::vector<float> samples;
            for (const auto& item : j["samples"]) {
                if (item.is_number()) {
                    float val = item.get<float>() / max_val;
                    samples.push_back(std::clamp(val, 0.0f, 1.0f));
                }
            }
            return samples;
        }
    } catch (...) {
        // Non-fatal
    }

    return std::vector<float>{};
}

core::Result<bool> SoundCloudApi::toggle_like_track(uint64_t track_id, bool current_liked_state) {
    if (auth_token_.empty()) {
        return core::MakeError(core::ErrorCode::AuthError, "OAuth token required to like tracks");
    }

    if (client_id_.empty()) {
        (void)resolve_client_id();
    }

    std::string url = std::string(ApiBase) + "/me/track_likes/" + std::to_string(track_id);
    if (!client_id_.empty()) {
        url += "?client_id=" + client_id_;
    }

    if (current_liked_state) {
        auto resp = http_.del(url);
        if (resp && resp->is_2xx()) {
            LOG_INFO("SoundCloudApi", "Successfully unliked track " + std::to_string(track_id));
            return false;
        }
        if (!resp) {
            LOG_ERROR("SoundCloudApi", "Failed to unlike track " + std::to_string(track_id) + ": " + resp.error().message);
            return core::MakeError(resp.error().code, resp.error().message, resp.error().http_status);
        }
        return core::MakeError(core::ErrorCode::HttpError, "Failed to unlike track HTTP " + std::to_string(resp->status_code), resp->status_code);
    } else {
        auto resp = http_.post(url, "{}");
        if (resp && resp->is_2xx()) {
            LOG_INFO("SoundCloudApi", "Successfully liked track " + std::to_string(track_id));
            return true;
        }
        if (!resp) {
            LOG_ERROR("SoundCloudApi", "Failed to like track " + std::to_string(track_id) + ": " + resp.error().message);
            return core::MakeError(resp.error().code, resp.error().message, resp.error().http_status);
        }
        return core::MakeError(core::ErrorCode::HttpError, "Failed to like track HTTP " + std::to_string(resp->status_code), resp->status_code);
    }
}

core::Result<std::vector<models::Track>> SoundCloudApi::parse_tracks_json(const std::string& json_str) {
    try {
        auto j = nlohmann::json::parse(json_str);
        std::vector<models::Track> tracks;

        const nlohmann::json* collection = nullptr;
        if (j.is_array()) {
            collection = &j;
        } else if (j.contains("collection") && j["collection"].is_array()) {
            collection = &j["collection"];
        } else if (j.contains("tracks") && j["tracks"].is_array()) {
            collection = &j["tracks"];
        }

        if (!collection) {
            return tracks;
        }

        for (const auto& item : *collection) {
            const nlohmann::json* track_obj = &item;
            if (item.contains("track") && item["track"].is_object()) {
                track_obj = &item["track"];
            }

            if (!track_obj->contains("id") || !(*track_obj)["id"].is_number()) {
                continue;
            }

            tracks.push_back(parse_track_object(*track_obj));
        }

        return tracks;
    } catch (const std::exception& e) {
        return core::MakeError(core::ErrorCode::JsonParseError, std::string("Error parsing tracks JSON: ") + e.what());
    }
}

core::Result<std::vector<models::Playlist>> SoundCloudApi::parse_playlists_json(const std::string& json_str) {
    try {
        auto j = nlohmann::json::parse(json_str);
        std::vector<models::Playlist> playlists;

        const nlohmann::json* collection = nullptr;
        if (j.is_array()) {
            collection = &j;
        } else if (j.contains("collection") && j["collection"].is_array()) {
            collection = &j["collection"];
        }

        if (!collection) return playlists;

        for (const auto& raw_item : *collection) {
            const nlohmann::json* item = &raw_item;
            if (raw_item.contains("playlist") && raw_item["playlist"].is_object()) {
                item = &raw_item["playlist"];
            }

            models::Playlist pl;
            if (item->contains("id") && (*item)["id"].is_number()) {
                pl.id = (*item)["id"].get<uint64_t>();
            }
            if (item->contains("title") && (*item)["title"].is_string()) {
                pl.title = (*item)["title"].get<std::string>();
            }
            if (item->contains("track_count") && (*item)["track_count"].is_number()) {
                pl.track_count = (*item)["track_count"].get<size_t>();
            }
            if (item->contains("duration") && (*item)["duration"].is_number()) {
                pl.duration_ms = (*item)["duration"].get<uint64_t>();
            }
            if (item->contains("likes_count") && (*item)["likes_count"].is_number()) {
                pl.likes_count = (*item)["likes_count"].get<uint64_t>();
            } else if (item->contains("favoritings_count") && (*item)["favoritings_count"].is_number()) {
                pl.likes_count = (*item)["favoritings_count"].get<uint64_t>();
            }
            playlists.push_back(std::move(pl));
        }

        return playlists;
    } catch (const std::exception& e) {
        return core::MakeError(core::ErrorCode::JsonParseError, std::string("Error parsing playlists JSON: ") + e.what());
    }
}

} // namespace sctui::net
