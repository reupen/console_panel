#pragma once

#define NOMINMAX

#include <algorithm>
#include <chrono>
#include <deque>
#include <mutex>
#include <vector>

#include "../ui_helpers/stdafx.h"

#include "../pfc/pfc.h"

#include <windows.h>
#include <commctrl.h>
#include <windowsx.h>

#include "../foobar2000/SDK/foobar2000.h"
#include "../columns_ui-sdk/ui_extension.h"

#include "version.h"

/**
 * This is the unique GUID identifying our panel. You should not re-use this one
 * and generate your own using GUIDGEN.
 */
constexpr GUID window_id{0x3c85d0a9, 0x19d5, 0x4144, {0xbc, 0xc2, 0x94, 0x9a, 0xb7, 0x64, 0x23, 0x3a}};

extern cfg_int cfg_last_edge_style;
extern cfg_bool cfg_last_hide_trailing_newline;

enum class EdgeStyle : int {
    None = 0,
    Sunken = 1,
    Grey = 2,
};

class Message {
public:
    std::chrono::system_clock::time_point m_timestamp;
    std::string m_message;

    Message(std::string message) : m_timestamp(std::chrono::system_clock::now()), m_message(std::move(message)) {}
};

class ConsoleWindow : public uie::container_uie_window_v3 {
public:
    static void s_update_all_fonts();
    static void s_update_colours();
    static void s_update_window_themes();
    static void s_on_message_received(std::string_view text); // from any thread

    const GUID& get_extension_guid() const override { return window_id; }
    void get_name(pfc::string_base& out) const override { out.set_string("Console"); }
    void get_category(pfc::string_base& out) const override { out.set_string("Panels"); }

    unsigned get_type() const override
    {
        /** In this case we are only of type type_panel */
        return ui_extension::type_panel;
    }

    uie::container_window_v3_config get_window_config() override { return {L"{89A3759F-348A-4e3f-BF43-3D16BC059186}"}; }

    void get_config(stream_writer* writer, abort_callback& abort) const override;
    void set_config(stream_reader* reader, t_size p_size, abort_callback& abort) override;

    void get_menu_items(uie::menu_hook_t& p_hook) override;

    long get_edit_ex_styles() const;
    void update_content();
    void update_content_throttled() noexcept;
    EdgeStyle get_edge_style() const { return m_edge_style; }
    void set_edge_style(EdgeStyle edge_style);
    bool get_hide_trailing_newline() const { return m_hide_trailing_newline; }
    void set_hide_trailing_newline(bool hide_trailing_newline);

protected:
    static void s_clear();

    LRESULT on_message(HWND wnd, UINT msg, WPARAM wp, LPARAM lp) override;
    std::optional<LRESULT> handle_edit_message(WNDPROC wnd_proc, HWND wnd, UINT msg, WPARAM wp, LPARAM lp);
    void set_window_theme() const;
    void copy() const;

    inline static std::mutex s_mutex;
    inline static wil::unique_hfont s_font;
    inline static wil::unique_hbrush s_background_brush;
    inline static std::deque<Message> s_messages;
    inline static std::vector<HWND> s_notify_list;
    inline static std::vector<service_ptr_t<ConsoleWindow>> s_windows;

    HWND m_wnd_edit{};
    std::chrono::steady_clock::time_point m_last_update_time_point;
    bool m_timer_active{};
    EdgeStyle m_edge_style{cfg_last_edge_style.get_value()};
    bool m_hide_trailing_newline{cfg_last_hide_trailing_newline.get_value()};
};
