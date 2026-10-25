#include "stdafx.h"

#include <algorithm>

#include <shlwapi.h>
#pragma comment(lib, "shlwapi.lib")

#include <helpers/DarkMode.h>
#include <helpers/atl-misc.h>

#include "config.h"
#include "core.h"
#include "maintenance.h"
#include "resource.h"
#include "time_format.h"

namespace upc_fb {

namespace {

struct candidate {
    std::string key;
    upc::record rec;
    std::string target; // current key of the same file (tags changed); empty = no track at all
    bool on_disk = false;
    bool keep = false;
};

std::wstring wide(const std::string& s) { return std::wstring(pfc::stringcvt::string_wide_from_utf8(s.c_str())); }

bool file_exists(const std::string& path) { return GetFileAttributesW(wide(path).c_str()) != INVALID_FILE_ATTRIBUTES; }

// The file is gone, and not just because its drive or share is offline.
bool file_gone(const upc::record& r) {
    if (r.path.empty() || file_exists(r.path)) return false;
    wchar_t root[MAX_PATH];
    const auto w = wide(r.path);
    if (w.size() >= MAX_PATH || wcscpy_s(root, w.c_str()) || !PathStripToRootW(root)) return false;
    return GetFileAttributesW(root) != INVALID_FILE_ATTRIBUTES;
}

bool has_value(const upc::record& r) { return r.playcount || r.rating || r.loved; }

class cleanup_dialog : public CDialogImpl<cleanup_dialog> {
public:
    enum { IDD = IDD_CLEANUP };

    cleanup_dialog(std::vector<candidate>& items, const std::string& summary) : m_items(items), m_summary(summary) {}

    BEGIN_MSG_MAP_EX(cleanup_dialog)
        MSG_WM_INITDIALOG(on_init)
        COMMAND_ID_HANDLER_EX(IDOK, on_close)
        COMMAND_ID_HANDLER_EX(IDCANCEL, on_close)
        COMMAND_ID_HANDLER_EX(IDC_KEEP_RATED, on_keep)
        COMMAND_ID_HANDLER_EX(IDC_KEEP_ALL, on_keep)
        COMMAND_ID_HANDLER_EX(IDC_KEEP_NONE, on_keep)
    END_MSG_MAP()

private:
    BOOL on_init(CWindow, LPARAM) {
        m_dark.AddDialogWithControls(*this);
        uSetDlgItemText(*this, IDC_CLEAN_SUMMARY, m_summary.c_str());
        m_list = GetDlgItem(IDC_CLEAN_LIST);
        m_list.SetExtendedListViewStyle(LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
        const struct {
            const wchar_t* name;
            int width;
        } cols[] = {{L"What", 90}, {L"Artist", 120}, {L"Title", 150}, {L"Album", 120}, {L"Plays", 45},
                    {L"Rating", 45}, {L"Last played", 115}, {L"File", 70}, {L"Path", 300}};
        for (int i = 0; i < static_cast<int>(std::size(cols)); ++i)
            m_list.InsertColumn(i, cols[i].name, LVCFMT_LEFT, cols[i].width);
        for (int i = 0; i < static_cast<int>(m_items.size()); ++i) {
            const auto& c = m_items[i];
            m_list.InsertItem(i, c.target.empty() ? L"No track" : L"Tags changed");
            const std::string texts[] = {c.rec.artist,
                                         c.rec.title,
                                         c.rec.album,
                                         std::to_string(c.rec.playcount),
                                         c.rec.rating ? std::to_string(c.rec.rating) : "",
                                         c.rec.last_played ? upc::format_date(c.rec.last_played) : "",
                                         c.on_disk ? "On disk" : "Missing",
                                         c.rec.path};
            for (int col = 0; col < static_cast<int>(std::size(texts)); ++col)
                m_list.SetItemText(i, col + 1, wide(texts[col]).c_str());
            m_list.SetCheckState(i, c.keep);
        }
        CenterWindow(GetParent());
        return TRUE;
    }

    void on_keep(UINT, int id, CWindow) {
        for (int i = 0; i < static_cast<int>(m_items.size()); ++i)
            m_list.SetCheckState(i, id == IDC_KEEP_ALL || (id == IDC_KEEP_RATED && has_value(m_items[i].rec)));
    }

    void on_close(UINT, int id, CWindow) {
        if (id == IDOK)
            for (int i = 0; i < static_cast<int>(m_items.size()); ++i) m_items[i].keep = m_list.GetCheckState(i) != FALSE;
        EndDialog(id);
    }

    std::vector<candidate>& m_items;
    const std::string m_summary;
    CListViewCtrl m_list;
    fb2k::CDarkModeHooks m_dark;
};

struct live_tracks {
    std::map<upc::location, std::string> keys;
    std::map<std::string, metadb_handle_ptr> handle_of_key;
};

live_tracks scan_live() {
    live_tracks live;
    for (const auto& track : known_tracks()) {
        std::string key;
        if (!key_of(track, key)) continue;
        live.keys[{native_path(track), track->get_subsong_index()}] = key;
        live.handle_of_key.emplace(key, track);
    }
    return live;
}

} // namespace

void run_cleanup() {
    if (!g_store.is_open()) return;
    const auto live = scan_live();
    const auto records = g_store.all();
    const auto scan = upc::scan_for_cleanup(records, live.keys);
    if (scan.reattach.empty() && scan.orphans.empty()) {
        popup_message::g_show(PFC_string_formatter() << "Nothing to clean: all " << records.size()
                                                     << " records belong to tracks foobar2000 knows.",
                              "Clean database");
        return;
    }

    std::map<std::string, const upc::record*> by_key;
    for (auto& [key, rec] : records) by_key.emplace(key, &rec);
    std::vector<candidate> items;
    for (auto& [from, to] : scan.reattach) items.push_back({from, *by_key[from], to, true, false});
    size_t valuable = 0, on_disk = 0;
    for (auto& key : scan.orphans) {
        candidate c{key, *by_key[key], {}, file_exists(by_key[key]->path)};
        c.keep = has_value(c.rec) || c.on_disk;
        valuable += has_value(c.rec);
        on_disk += c.on_disk;
        items.push_back(std::move(c));
    }
    std::stable_sort(items.begin() + scan.reattach.size(), items.end(), [](const candidate& a, const candidate& b) {
        return a.rec.artist != b.rec.artist ? a.rec.artist < b.rec.artist : a.rec.title < b.rec.title;
    });

    const pfc::string8 summary = PFC_string_formatter()
                                << records.size() << " records. " << scan.reattach.size()
                                << " belong to files whose tags changed (they can be merged into the current track). "
                                << scan.orphans.size() << " have no track in foobar2000: " << valuable
                                << " of them with plays or ratings, " << on_disk
                                << " with the file still on disk (only not in the library or playlists).";
    cleanup_dialog dialog(items, summary.c_str());
    if (dialog.DoModal(core_api::get_main_window()) != IDOK) return;

    std::map<std::string, upc::record> merged;
    std::vector<std::string> removes;
    std::vector<std::pair<std::string, std::string>> moves;
    size_t merged_from = 0;
    metadb_handle_list refresh;
    for (const auto& c : items) {
        if (c.keep) continue;
        removes.push_back(c.key);
        if (c.target.empty()) continue;
        ++merged_from;
        moves.emplace_back(c.key, c.target);
        auto [it, fresh] = merged.try_emplace(c.target);
        if (fresh) {
            it->second = g_store.get(c.target).value_or(upc::record{});
            const auto& track = live.handle_of_key.at(c.target);
            set_label(it->second, track);
            refresh.add_item(track);
        }
        it->second = upc::combine(it->second, c.rec);
    }
    if (removes.empty()) return;
    pfc::string8 backup;
    try {
        backup = make_backup("clean");
        g_store.put_many({merged.begin(), merged.end()}, removes, moves);
    } catch (const std::exception& e) {
        popup_message::g_complain("Ultimate Playcount: cleaning failed, nothing was changed", e);
        return;
    }
    if (refresh.get_count()) metadb_io::get()->dispatch_refresh(refresh);
    popup_message::g_show(PFC_string_formatter() << "Merged " << merged_from << " records into current tracks, removed "
                                                 << removes.size() - merged_from << " other records.\n\nBackup: "
                                                 << backup,
                          "Clean database");
}

void run_auto_clean() {
    if (!g_store.is_open() || !upc_cfg::auto_clean.get()) return;
    const auto live = scan_live();
    const auto records = g_store.all();
    const auto scan = upc::scan_for_cleanup(records, live.keys);
    std::map<std::string, const upc::record*> by_key;
    for (auto& [key, rec] : records) by_key.emplace(key, &rec);
    std::vector<std::pair<std::string, upc::record>> orphans;
    for (auto& key : scan.orphans) orphans.emplace_back(key, *by_key[key]);
    const auto days = static_cast<int>(upc_cfg::auto_clean_days.get());
    const auto picked = upc::pick_auto_clean(orphans, records.size(), unix_now(), days, file_gone);
    if (picked.empty()) return;
    try {
        const auto backup = make_backup("autoclean");
        g_store.put_many({}, picked);
        FB2K_console_formatter() << log_prefix << "Removed " << picked.size() << " records of files deleted more than "
                                 << days << " days ago. Backup: " << backup;
    } catch (const std::exception& e) {
        FB2K_console_formatter() << log_prefix << "Automatic cleanup failed, nothing was changed: " << e.what();
    }
}

} // namespace upc_fb
