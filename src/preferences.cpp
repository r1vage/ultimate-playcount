#include "stdafx.h"

#include <algorithm>

#include <helpers/DarkMode.h>
#include <helpers/atl-misc.h>

#include "config.h"
#include "resource.h"

namespace upc_cfg {
cfg_int count_mode({0x3bc6ad8e, 0x5803, 0x4044, {0xbd, 0x56, 0x4d, 0x47, 0xcf, 0x8a, 0xd8, 0x7c}}, default_count_mode);
cfg_int count_percent({0xc12315fe, 0xdc54, 0x4906, {0x93, 0x12, 0xca, 0x15, 0x5b, 0x6f, 0x31, 0x92}}, default_count_percent);
cfg_int count_seconds({0x3f220569, 0x3e9f, 0x4747, {0x90, 0x0b, 0xb9, 0x47, 0x85, 0x6f, 0xf4, 0xcb}}, default_count_seconds);
cfg_string key({0xb5965c53, 0x247b, 0x4994, {0x90, 0x6d, 0x15, 0x02, 0x19, 0x28, 0x11, 0x90}}, default_key);
cfg_bool ignore_case({0x6e2d9a41, 0x3c7b, 0x4f15, {0xa8, 0x52, 0x1d, 0x9e, 0x7c, 0x30, 0xb4, 0x6f}}, true);
cfg_bool follow_edits({0xd41f7b26, 0x85a3, 0x4e09, {0x9c, 0x6d, 0x2b, 0xe8, 0x51, 0x47, 0x0a, 0x93}}, true);
cfg_string prefix({0x0a3143ed, 0xa6a0, 0x4012, {0x90, 0x0a, 0x99, 0xf6, 0x36, 0xa4, 0xcf, 0x31}}, default_prefix);
cfg_bool legacy_names({0x89867731, 0x1113, 0x43b5, {0x9b, 0x35, 0x45, 0x35, 0x64, 0x5e, 0x6a, 0x1b}}, false);
cfg_bool lfm_names({0x91c7d3b2, 0x58e4, 0x4a1f, {0x8e, 0x25, 0xc6, 0x0b, 0x3d, 0x74, 0xf1, 0xa9}}, false);
cfg_bool enhanced_names({0x4e7a1c92, 0x6b30, 0x4d58, {0x9a, 0x0f, 0x31, 0xc8, 0x5d, 0xe2, 0x77, 0xb1}}, false);
cfg_string listenbrainz_user({0x2b6e9f14, 0x70c3, 0x4d85, {0x93, 0xa8, 0x1e, 0x5c, 0xf0, 0x47, 0xb2, 0xd6}}, "");
cfg_string lastfm_user({0x7a2e5d10, 0x3c84, 0x4b97, {0xa1, 0x6e, 0x58, 0x0d, 0x92, 0xf3, 0xc4, 0x1a}}, "");
cfg_string lastfm_key({0xc19d63f7, 0x28ab, 0x4e05, {0x8d, 0x72, 0x1f, 0xa9, 0x40, 0xb6, 0x3e, 0x85}}, "");
cfg_int lastfm_summary_every({0x3f9b6d52, 0xa714, 0x4c08, {0x9e, 0x31, 0x62, 0xd8, 0x0b, 0xf5, 0x47, 0xa3}}, 50);
cfg_bool lastfm_update({0x8c41f2d7, 0x1a63, 0x4e90, {0xb5, 0x08, 0x37, 0xd2, 0x9f, 0x60, 0xa4, 0x1e}}, true);
cfg_bool lastfm_on_end({0x24c671b2, 0x86ee, 0x4915, {0xa3, 0x78, 0xb0, 0x42, 0xf7, 0xd4, 0x40, 0x74}}, true);
cfg_bool lastfm_background({0xc8dc99e9, 0x087c, 0x4efb, {0x9e, 0x11, 0x9b, 0xc8, 0x48, 0xa3, 0xdf, 0xca}}, true);
cfg_bool lastfm_feeds({0x05b8e4a3, 0x91d6, 0x4c2f, {0xb7, 0x30, 0xe2, 0x6a, 0x1c, 0x84, 0xd9, 0x5f}}, false);
cfg_bool count_skips({0x6a4d0e91, 0xb2f7, 0x4c35, {0x8a, 0x19, 0xd3, 0x72, 0x05, 0xe8, 0x4b, 0xc6}}, false);
cfg_bool ignore_outside({0xde05affe, 0xe703, 0x44f0, {0x85, 0xe5, 0xa1, 0xfb, 0x05, 0xd2, 0xac, 0x9f}}, false);
cfg_bool debug({0x1d38d162, 0x2859, 0x4fde, {0xab, 0x2c, 0x33, 0xb0, 0xcc, 0x5a, 0x26, 0x1d}}, false);
cfg_bool log_events({0xb84b25e8, 0x9ac4, 0x4a93, {0x8d, 0x4f, 0xec, 0xda, 0x3d, 0xc6, 0x08, 0xa2}}, false);
cfg_bool auto_clean({0x09ba53a8, 0x525d, 0x492b, {0x9b, 0x83, 0x56, 0x51, 0x5d, 0x3e, 0xfe, 0x40}}, false);
cfg_int auto_clean_days({0x59fef82c, 0x6e88, 0x4aa2, {0xbd, 0x45, 0xc6, 0x53, 0x05, 0xba, 0xd4, 0x53}}, 365);
cfg_bool auto_backup({0xdcefb83a, 0xff8b, 0x4a03, {0x87, 0xa1, 0x23, 0x8b, 0x35, 0x05, 0xaa, 0xa0}}, true);
cfg_int auto_backup_days({0x20cc7871, 0x526f, 0x4b6d, {0xaf, 0x10, 0x04, 0x93, 0x71, 0x3a, 0x82, 0xc7}}, 1);
cfg_int backup_keep({0x5aa7d9c8, 0x172f, 0x4934, {0x9f, 0xef, 0x6c, 0x60, 0x1b, 0xe7, 0x5a, 0xcf}}, 10);
} // namespace upc_cfg

namespace {

using namespace upc_cfg;

struct {
    pfc::string8 key, prefix;
    bool legacy_names = false, enhanced_names = false, lfm_names = false, ignore_case = true;
} g_startup;
FB2K_RUN_ON_INIT([] {
    g_startup.key = key.get();
    g_startup.prefix = prefix.get();
    g_startup.legacy_names = legacy_names.get();
    g_startup.enhanced_names = enhanced_names.get();
    g_startup.lfm_names = lfm_names.get();
    g_startup.ignore_case = ignore_case.get();
});

// Every setting on the page, once: control, stored value, default.
struct text_setting {
    int id;
    cfg_string& var;
    const char* def;
};
struct check_setting {
    int id;
    cfg_bool& var;
    bool def;
};
struct number_setting {
    int id;
    cfg_int& var;
    int def, min, max; // an empty or out-of-range box means the default / nearest limit
};
const text_setting texts[] = {{IDC_KEY, key, default_key},
                              {IDC_PREFIX, prefix, default_prefix},
                              {IDC_LB_USER, listenbrainz_user, ""},
                              {IDC_LASTFM_USER, lastfm_user, ""},
                              {IDC_LASTFM_KEY, lastfm_key, ""}};
const check_setting checks[] = {{IDC_IGNORE_OUTSIDE, ignore_outside, false}, {IDC_COUNT_SKIPS, count_skips, false}, {IDC_DEBUG, debug, false},
                                {IDC_LOG_EVENTS, log_events, false},         {IDC_LEGACY_NAMES, legacy_names, false},
                                {IDC_ENHANCED_NAMES, enhanced_names, false},
                                {IDC_LFM_NAMES, lfm_names, false},
                                {IDC_LASTFM_FEEDS, lastfm_feeds, false},
                                {IDC_LASTFM_UPDATE, lastfm_update, true},
                                {IDC_LASTFM_ON_END, lastfm_on_end, true},
                                {IDC_LASTFM_BACKGROUND, lastfm_background, true},
                                {IDC_AUTO_CLEAN, auto_clean, false},         {IDC_AUTO_BACKUP, auto_backup, true},
                                {IDC_IGNORE_CASE, ignore_case, true},        {IDC_FOLLOW_EDITS, follow_edits, true}};
const number_setting numbers[] = {{IDC_PERCENT, count_percent, default_count_percent, 1, 100},
                                  {IDC_SECONDS, count_seconds, default_count_seconds, 1, 86400},
                                  {IDC_AUTO_CLEAN_DAYS, auto_clean_days, 365, 1, 100000},
                                  {IDC_AUTO_BACKUP_DAYS, auto_backup_days, 1, 1, 365},
                                  {IDC_BACKUP_KEEP, backup_keep, 10, 1, 1000},
                                  {IDC_LASTFM_SUMMARY, lastfm_summary_every, 50, 1, 100000}};
constexpr int mode_ids[] = {IDC_MODE_PERCENT, IDC_MODE_SECONDS, IDC_MODE_END}; // index = upc::count_mode

// One dialog class for all three pages: each page's dialog resource holds only its own controls, and every
// setting whose control isn't on the page is skipped.
template <int Dialog> class page_dialog : public CDialogImpl<page_dialog<Dialog>>, public preferences_page_instance {
public:
    enum { IDD = Dialog };

    page_dialog(preferences_page_callback::ptr callback) : m_callback(callback) {}

    t_uint32 get_state() override {
        t_uint32 state = preferences_state::resettable | preferences_state::dark_mode_supported;
        if (changed()) state |= preferences_state::changed;
        if (restart_changed()) state |= preferences_state::needs_restart;
        return state;
    }

    void apply() override {
        for (auto& s : texts)
            if (has(s.id)) s.var.set(text(s.id));
        for (auto& s : checks)
            if (has(s.id)) s.var.set(checked(s.id));
        for (auto& s : numbers)
            if (has(s.id)) s.var.set(number(s));
        if (has(mode_ids[0])) count_mode.set(mode());
        m_callback->on_state_changed();
    }

    void reset() override {
        for (auto& s : texts)
            if (has(s.id)) uSetDlgItemText(*this, s.id, s.def);
        for (auto& s : checks)
            if (has(s.id)) this->CheckDlgButton(s.id, s.def);
        for (auto& s : numbers)
            if (has(s.id)) this->SetDlgItemInt(s.id, s.def, FALSE);
        if (has(mode_ids[0])) show_mode(default_count_mode);
        m_callback->on_state_changed();
    }

    BEGIN_MSG_MAP_EX(page_dialog)
        MSG_WM_INITDIALOG(on_init)
        COMMAND_CODE_HANDLER_EX(EN_CHANGE, on_change)
        COMMAND_CODE_HANDLER_EX(BN_CLICKED, on_change)
    END_MSG_MAP()

private:
    BOOL on_init(CWindow, LPARAM) {
        m_dark.AddDialogWithControls(*this);
        for (auto& s : texts)
            if (has(s.id)) uSetDlgItemText(*this, s.id, s.var.get());
        for (auto& s : checks)
            if (has(s.id)) this->CheckDlgButton(s.id, s.var.get());
        for (auto& s : numbers)
            if (has(s.id)) this->SetDlgItemInt(s.id, static_cast<UINT>(s.var.get()), FALSE);
        if (has(mode_ids[0])) show_mode(static_cast<int>(count_mode.get()));
        return FALSE;
    }

    void on_change(UINT, int id, CWindow) {
        // The mode radios sit between other controls, so Windows can't group them: tick the clicked one here.
        for (int i = 0; i < static_cast<int>(std::size(mode_ids)); ++i)
            if (id == mode_ids[i]) show_mode(i);
        m_callback->on_state_changed();
    }

    void show_mode(int mode) {
        for (int i = 0; i < static_cast<int>(std::size(mode_ids)); ++i) this->CheckDlgButton(mode_ids[i], i == mode);
    }
    int mode() {
        for (int i = 0; i < static_cast<int>(std::size(mode_ids)); ++i)
            if (checked(mode_ids[i])) return i;
        return default_count_mode;
    }

    bool has(int id) { return this->GetDlgItem(id) != NULL; }
    pfc::string8 text(int id) { return uGetDlgItemText(*this, id); }
    bool checked(int id) { return this->IsDlgButtonChecked(id) == BST_CHECKED; }
    int64_t number(const number_setting& s) {
        BOOL ok = FALSE;
        const UINT v = this->GetDlgItemInt(s.id, &ok, FALSE);
        if (!ok) return s.def;
        return std::clamp<int64_t>(v, s.min, s.max);
    }

    // Against the values in use since startup, so the restart hint stays after Apply.
    bool restart_changed() {
        return has(IDC_KEY) && (text(IDC_KEY) != g_startup.key || text(IDC_PREFIX) != g_startup.prefix ||
                                checked(IDC_LEGACY_NAMES) != g_startup.legacy_names ||
                                checked(IDC_ENHANCED_NAMES) != g_startup.enhanced_names ||
                                checked(IDC_LFM_NAMES) != g_startup.lfm_names ||
                                checked(IDC_IGNORE_CASE) != g_startup.ignore_case);
    }
    bool changed() {
        for (auto& s : texts)
            if (has(s.id) && text(s.id) != s.var.get()) return true;
        for (auto& s : checks)
            if (has(s.id) && checked(s.id) != s.var.get()) return true;
        for (auto& s : numbers)
            if (has(s.id) && number(s) != s.var.get()) return true;
        return has(mode_ids[0]) && mode() != count_mode.get();
    }

    const preferences_page_callback::ptr m_callback;
    fb2k::CDarkModeHooks m_dark;
};

constexpr GUID guid_page_main = {0xf3a1c2d4, 0x6b7e, 0x4c58, {0x9a, 0x0b, 0x1e, 0x2f, 0x3a, 0x4b, 0x5c, 0x6d}};

class page_main : public preferences_page_impl<page_dialog<IDD_PREFERENCES>> {
public:
    const char* get_name() override { return "Ultimate Playcount"; }
    GUID get_guid() override { return guid_page_main; }
    GUID get_parent_guid() override { return guid_tools; }
};
preferences_page_factory_t<page_main> g_page_main;

class page_records : public preferences_page_impl<page_dialog<IDD_PREFS_RECORDS>> {
public:
    const char* get_name() override { return "Records and fields"; }
    GUID get_guid() override { return {0x7ce0ea6a, 0x66bc, 0x4826, {0x97, 0x92, 0x04, 0x6c, 0x80, 0x9b, 0x91, 0xde}}; }
    GUID get_parent_guid() override { return guid_page_main; }
};
preferences_page_factory_t<page_records> g_page_records;

class page_maintenance : public preferences_page_impl<page_dialog<IDD_PREFS_MAINTENANCE>> {
public:
    const char* get_name() override { return "Cleanup and backups"; }
    GUID get_guid() override { return {0x92374f2c, 0xc22c, 0x4985, {0xb2, 0xca, 0xba, 0x0a, 0x35, 0x0f, 0xdd, 0xcc}}; }
    GUID get_parent_guid() override { return guid_page_main; }
};
preferences_page_factory_t<page_maintenance> g_page_maintenance;

class page_lastfm : public preferences_page_impl<page_dialog<IDD_PREFS_LASTFM>> {
public:
    const char* get_name() override { return "Last.fm"; }
    GUID get_guid() override { return {0x3d8f1b64, 0xe7a2, 0x4c09, {0x95, 0x1b, 0x6a, 0x72, 0xd0, 0x4e, 0x38, 0xc5}}; }
    GUID get_parent_guid() override { return guid_page_main; }
};
preferences_page_factory_t<page_lastfm> g_page_lastfm;

class page_listenbrainz : public preferences_page_impl<page_dialog<IDD_PREFS_LISTENBRAINZ>> {
public:
    const char* get_name() override { return "ListenBrainz"; }
    GUID get_guid() override { return {0xd5a07c38, 0x1f92, 0x4e6b, {0xa4, 0x17, 0x8b, 0x30, 0xe9, 0x6d, 0x52, 0xc1}}; }
    GUID get_parent_guid() override { return guid_page_main; }
};
preferences_page_factory_t<page_listenbrainz> g_page_listenbrainz;

} // namespace
