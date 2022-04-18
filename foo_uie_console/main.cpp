/**
 * \file main.cpp
 *
 * \brief Console panel component
 *
 * This component is an example of a multiple instance panel that takes keyboard input
 *
 * It demonstrates the following relevant techniques:
 * - Subclassing the child control to process keyboard shortcuts
 * - Setting the font and colours of the child window
 * - Keeping a list of active windows and updating them from a callback (in this case designed
 *   such that the callback may come from any thread)
 * - That's about it ?
 */

#define NOMINMAX

#include <algorithm>
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

/** Declare some component information */
DECLARE_COMPONENT_VERSION("Console panel", "3.0.0-beta.1",
    "compiled: " __DATE__ "\n"
    "with Panel API version: " UI_EXTENSION_VERSION

);

constexpr auto IDC_EDIT = 1001;
constexpr auto MSG_UPDATE = WM_USER + 2;
constexpr auto ID_TIMER = 667;

/** \brief The maximum number of message we cache/display */
constexpr t_size maximum_messages = 200;

enum class EdgeStyle : int {
    None = 0,
    Sunken = 1,
    Grey = 2,
};

cfg_int cfg_last_edge_style(GUID{0x05550547, 0xbf98, 0x088c, 0xbe, 0x0e, 0x24, 0x95, 0xe4, 0x9b, 0x88, 0xc7},
    static_cast<int>(EdgeStyle::None));

cfg_bool cfg_last_hide_trailing_newline(
    {0x5db0b4d6, 0xf429, 0x4fc5, 0xb9, 0x1d, 0x29, 0x8e, 0xf3, 0x34, 0x75, 0x16}, false);

constexpr GUID console_font_id = {0x26059feb, 0x488b, 0x4ce1, {0x82, 0x4e, 0x4d, 0xf1, 0x13, 0xb4, 0x55, 0x8e}};

constexpr GUID console_colours_client_id
    = {0x9d814898, 0x0db4, 0x4591, {0xa7, 0xaa, 0x4e, 0x94, 0xdd, 0x07, 0xb3, 0x87}};

/**
 * This is the unique GUID identifying our panel. You should not re-use this one
 * and generate your own using GUIDGEN.
 */
constexpr GUID window_id{0x3c85d0a9, 0x19d5, 0x4144, {0xbc, 0xc2, 0x94, 0x9a, 0xb7, 0x64, 0x23, 0x3a}};
constexpr auto current_config_version = 0;

class Message {
public:
    SYSTEMTIME m_time{};
    std::string m_message;

    Message(std::string_view message) : m_message(message) { GetLocalTime(&m_time); }
};

class ConsoleWindow : public uie::container_uie_window_v3 {
public:
    static void s_update_all_fonts();
    static void s_update_colours();
    static void s_update_window_themes();
    static void s_on_message_received(const char* ptr, t_size len); // from any thread

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
    void update_content_throttled();
    EdgeStyle get_edge_style() const { return m_edge_style; }
    void set_edge_style(EdgeStyle edge_style);
    bool get_hide_trailing_newline() const { return m_hide_trailing_newline; }
    void set_hide_trailing_newline(bool hide_trailing_newline);

private:
    static LRESULT WINAPI hook_proc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp);
    static void s_clear();

    LRESULT on_message(HWND wnd, UINT msg, WPARAM wp, LPARAM lp) override;
    LRESULT on_hook(HWND wnd, UINT msg, WPARAM wp, LPARAM lp);
    void set_window_theme() const;
    void copy() const;

    static std::mutex s_mutex;
    static HFONT s_font;
    static wil::unique_hbrush s_background_brush;
    static std::deque<Message> s_messages;
    static std::vector<HWND> s_notify_list;
    static std::vector<service_ptr_t<ConsoleWindow>> s_windows;

    HWND m_wnd_edit{};
    WNDPROC m_editproc{};
    LARGE_INTEGER m_time_last_update{};
    bool m_timer_active{};
    EdgeStyle m_edge_style{cfg_last_edge_style.get_value()};
    bool m_hide_trailing_newline{cfg_last_hide_trailing_newline.get_value()};
};

std::vector<HWND> ConsoleWindow::s_notify_list;
std::vector<service_ptr_t<ConsoleWindow>> ConsoleWindow::s_windows;
HFONT ConsoleWindow::s_font{};
wil::unique_hbrush ConsoleWindow::s_background_brush;
std::deque<Message> ConsoleWindow::s_messages;
std::mutex ConsoleWindow::s_mutex;

void ConsoleWindow::s_update_all_fonts()
{
    if (s_font != nullptr) {
        for (auto&& window : s_windows) {
            const HWND wnd = window->m_wnd_edit;
            if (wnd)
                SetWindowFont(wnd, nullptr, FALSE);
        }
        DeleteObject(s_font);
    }

    s_font = cui::fonts::helper(console_font_id).get_font();

    for (auto&& window : s_windows) {
        const HWND wnd = window->m_wnd_edit;
        if (wnd) {
            SetWindowFont(wnd, s_font, TRUE);
        }
    }
}

void ConsoleWindow::s_update_colours()
{
    s_background_brush.reset(
        CreateSolidBrush(cui::colours::helper(console_colours_client_id).get_colour(cui::colours::colour_background)));

    for (auto&& window : s_windows) {
        const HWND wnd = window->m_wnd_edit;
        if (wnd)
            RedrawWindow(wnd, nullptr, nullptr, RDW_INVALIDATE);
    }
}

void ConsoleWindow::s_update_window_themes()
{
    for (auto&& window : s_windows) {
        window->set_window_theme();
    }
}

void ConsoleWindow::set_edge_style(EdgeStyle edge_style)
{
    m_edge_style = edge_style;
    cfg_last_edge_style = static_cast<int32_t>(edge_style);

    const auto flags = get_edit_ex_styles();

    if (m_wnd_edit) {
        SetWindowLongPtr(m_wnd_edit, GWL_EXSTYLE, flags);
        SetWindowPos(m_wnd_edit, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
    }
}

void ConsoleWindow::set_hide_trailing_newline(bool hide_trailing_newline)
{
    cfg_last_hide_trailing_newline = m_hide_trailing_newline = hide_trailing_newline;

    update_content_throttled();
}

void ConsoleWindow::s_on_message_received(const char* ptr, t_size len)
{
    std::scoped_lock<std::mutex> _(s_mutex);

    pfc::string8 buffer;
    /**  Sort out line break messes */
    {
        const char* start = ptr;
        const char* pos = ptr;
        while (t_size(pos - ptr) < len && *pos) {
            while (t_size(pos - ptr + 1) < len && pos[1] && pos[0] != '\n')
                pos++;
            {
                if (pos[0] == '\n')
                    buffer.add_string(start, pos - start - ((pos > ptr && (pos[-1]) == '\r') ? 1 : 0));
                else
                    buffer.add_string(start, pos + 1 - start);
                buffer.add_byte('\r');
                buffer.add_byte('\n');
                // if ((pos-ptr)<len && *pos)
                {
                    start = pos + 1;
                    pos++;
                }
            }
        }
    }
    s_messages.emplace_back(Message({buffer.get_ptr(), buffer.get_length()}));
    if (s_messages.size() == maximum_messages)
        s_messages.pop_front();

    /** Post a notification to all instances of the panel to update their display */
    for (auto&& wnd : s_notify_list) {
        PostMessage(wnd, MSG_UPDATE, 0, 0);
    }
}

void ConsoleWindow::copy() const
{
    DWORD start{};
    DWORD end{};
    SendMessage(m_wnd_edit, EM_GETSEL, reinterpret_cast<WPARAM>(&start), reinterpret_cast<LPARAM>(&end));

    const auto has_selection = start != end;

    if (has_selection) {
        SendMessage(m_wnd_edit, WM_COPY, NULL, NULL);
    } else {
        const auto text = uGetWindowText(m_wnd_edit);
        uih::set_clipboard_text(text.get_ptr());
    }
}

void ConsoleWindow::s_clear()
{
    std::scoped_lock<std::mutex> _(s_mutex);

    /** Clear all messages */
    s_messages.clear();

    /** Post a notification to all instances of the panel to update their display */
    for (auto&& wnd : s_notify_list) {
        PostMessage(wnd, MSG_UPDATE, 0, 0);
    }
}

void ConsoleWindow::get_config(stream_writer* writer, abort_callback& abort) const
{
    writer->write_lendian_t(current_config_version, abort);
    writer->write_lendian_t(static_cast<int32_t>(m_edge_style), abort);
    writer->write_object_t(m_hide_trailing_newline, abort);
}

void ConsoleWindow::set_config(stream_reader* reader, t_size p_size, abort_callback& abort)
{
    int32_t version{};
    try {
        reader->read_lendian_t(version, abort);
    } catch (const exception_io_data_truncation&) {
        return;
    }

    if (version <= current_config_version) {
        int32_t edge_style{};
        reader->read_lendian_t(edge_style, abort);
        m_edge_style = EdgeStyle{edge_style};

        // Gracefully handle the new 'hide trailing newline' config bits when updating the component
        try {
            reader->read_object_t(m_hide_trailing_newline, abort);
        } catch (const exception_io_data_truncation&) {
        }
    }
}

class EdgeStyleMenuNode : public uie::menu_node_popup_t {
public:
    EdgeStyleMenuNode(service_ptr_t<ConsoleWindow> window)
    {
        const auto current_edge_style = window->get_edge_style();

        m_nodes.emplace_back("None", "Set the edge style to 'None'",
            current_edge_style == EdgeStyle::None ? state_radiochecked : 0,
            [window] { window->set_edge_style(EdgeStyle::None); });

        m_nodes.emplace_back("Sunken", "Set the edge style to 'Sunken'",
            current_edge_style == EdgeStyle::Sunken ? state_radiochecked : 0,
            [window] { window->set_edge_style(EdgeStyle::Sunken); });

        m_nodes.emplace_back("Grey", "Set the edge style to 'Grey'",
            current_edge_style == EdgeStyle::Grey ? state_radiochecked : 0,
            [window] { window->set_edge_style(EdgeStyle::Grey); });
    }

    t_size get_children_count() const override { return m_nodes.size(); }
    void get_child(t_size index, uie::menu_node_ptr& p_out) const override
    {
        if (index < m_nodes.size())
            p_out = new uie::simple_command_menu_node(m_nodes[index]);
    }
    bool get_display_data(pfc::string_base& p_out, unsigned& p_state) const override
    {
        p_out = "Edge style";
        return true;
    }

private:
    std::vector<uie::simple_command_menu_node> m_nodes;
};

void ConsoleWindow::get_menu_items(uie::menu_hook_t& p_hook)
{
    p_hook.add_node(new EdgeStyleMenuNode(this));
    p_hook.add_node(new uie::simple_command_menu_node("Hide trailing newline",
        "Toggles visibility of the trailing newline.", get_hide_trailing_newline() ? uih::Menu::flag_checked : 0,
        [this, self = ptr{this}] { set_hide_trailing_newline(!get_hide_trailing_newline()); }));
}

long ConsoleWindow::get_edit_ex_styles() const
{
    if (m_edge_style == EdgeStyle::Sunken)
        return WS_EX_CLIENTEDGE;

    if (m_edge_style == EdgeStyle::Grey)
        return WS_EX_STATICEDGE;

    return 0;
}

void ConsoleWindow::update_content()
{
    std::scoped_lock<std::mutex> _(s_mutex);
    pfc::string8_fastalloc buffer;
    buffer.prealloc(1024);

    for (auto&& message : s_messages) {
        buffer << "[" << pfc::format_int(message.m_time.wHour, 2) << ":" << pfc::format_int(message.m_time.wMinute, 2)
               << ":" << pfc::format_int(message.m_time.wSecond, 2) << "] " << message.m_message.c_str();
    }

    if (m_hide_trailing_newline && !buffer.is_empty())
        buffer.truncate(std::string_view(buffer.c_str()).find_last_not_of("\r\n") + 1);

    uSetWindowText(m_wnd_edit, buffer);
    const int len = Edit_GetLineCount(m_wnd_edit);
    Edit_Scroll(m_wnd_edit, len, 0);
    QueryPerformanceCounter(&m_time_last_update);
}

void ConsoleWindow::update_content_throttled()
{
    if (m_timer_active)
        return;

    LARGE_INTEGER current = {0}, freq = {0};
    QueryPerformanceCounter(&current);
    QueryPerformanceFrequency(&freq);
    t_uint64 tenth = 5;
    if (m_time_last_update.QuadPart) {
        tenth = (current.QuadPart - m_time_last_update.QuadPart) / (freq.QuadPart / 100);
    }
    if (tenth < 25) {
        SetTimer(get_wnd(), ID_TIMER, 250 - t_uint32(tenth) * 10, nullptr);
        m_timer_active = true;
    } else
        update_content();
}

LRESULT ConsoleWindow::on_message(HWND wnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE: {
        /**
         * Store a pointer to ourselves in this list, used for global notifications (in the main thread)
         * which updates instances of our panel.
         */
        s_windows.emplace_back(this);
        {
            std::scoped_lock<std::mutex> _(s_mutex);
            /** Store a window handle in this list, used in global notifications (in any thread) which
             * updates the panels */
            s_notify_list.emplace_back(wnd);
        }

        const auto edit_ex_styles = get_edit_ex_styles();

        /** Create our edit window */
        m_wnd_edit = CreateWindowEx(edit_ex_styles, WC_EDIT, _T(""),
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOVSCROLL | WS_VSCROLL | ES_READONLY | ES_MULTILINE, 0, 0, 0, 0,
            wnd, HMENU(IDC_EDIT), core_api::get_my_instance(), nullptr);

        if (m_wnd_edit) {
            set_window_theme();

            if (s_font) {
                /** Nth, n>1, instance; use exisiting font handle */
                SetWindowFont(m_wnd_edit, s_font, FALSE);
            } else
                /** First window - create the font handle */
                s_update_all_fonts();

            if (!s_background_brush)
                s_update_colours();

            /** Store a pointer to ourself in the user data field of the edit window */
            SetWindowLongPtr(m_wnd_edit, GWL_USERDATA, reinterpret_cast<LPARAM>(this));
            /** Subclass the edit window */
            m_editproc = reinterpret_cast<WNDPROC>(
                SetWindowLongPtr(m_wnd_edit, GWL_WNDPROC, reinterpret_cast<LPARAM>(hook_proc)));

            SendMessage(wnd, MSG_UPDATE, 0, 0);
        }
    } break;
    case WM_TIMER:
        if (wp == ID_TIMER) {
            KillTimer(wnd, ID_TIMER);
            m_timer_active = false;
            update_content();
            return 0;
        }
        break;
    /** Update the edit window's text */
    case MSG_UPDATE:
        update_content_throttled();
        break;
    case WM_SIZE:
        /** Reposition the edit window. */
        SetWindowPos(m_wnd_edit, nullptr, 0, 0, LOWORD(lp), HIWORD(lp), SWP_NOZORDER);
        break;
    case WM_CTLCOLORSTATIC: {
        const auto dc = reinterpret_cast<HDC>(wp);

        cui::colours::helper helper(console_colours_client_id);

        SetTextColor(dc, helper.get_colour(cui::colours::colour_text));
        SetBkColor(dc, helper.get_colour(cui::colours::colour_background));

        return reinterpret_cast<LRESULT>(s_background_brush.get());
    }
    case WM_ERASEBKGND:
        return FALSE;
    case WM_DESTROY:
        m_wnd_edit = nullptr;
        s_windows.erase(std::remove(s_windows.begin(), s_windows.end(), this), s_windows.end());

        {
            std::scoped_lock<std::mutex> _(s_mutex);
            s_notify_list.erase(std::remove(s_notify_list.begin(), s_notify_list.end(), wnd), s_notify_list.end());
        }
        break;
    case WM_NCDESTROY:
        if (s_windows.empty()) {
            DeleteFont(s_font);
            s_font = nullptr;
            s_background_brush.reset();
        }
        break;
    }
    return DefWindowProc(wnd, msg, wp, lp);
}

LRESULT WINAPI ConsoleWindow::hook_proc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp)
{
    auto* self = reinterpret_cast<ConsoleWindow*>(GetWindowLongPtr(wnd, GWL_USERDATA));
    return self ? self->on_hook(wnd, msg, wp, lp) : DefWindowProc(wnd, msg, wp, lp);
}

LRESULT ConsoleWindow::on_hook(HWND wnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_ERASEBKGND:
        return FALSE;
    case WM_PAINT:
        uih::paint_subclassed_window_with_buffering(wnd, m_editproc);
        return 0;
    case WM_KEYDOWN:
        /**
         * It's possible to assign right, left, up and down keys to keyboard shortcuts. But we would rather
         * let the edit control process those.
         */
        if (get_host()->get_keyboard_shortcuts_enabled() && wp != VK_LEFT && wp != VK_RIGHT && wp != VK_UP
            && wp != VK_DOWN && g_process_keydown_keyboard_shortcuts(wp)) {
            return 0;
        }
        if (wp == VK_TAB) {
            g_on_tab(wnd);
            return 0;
        }
        break;
    case WM_SYSKEYDOWN:
        if (get_host()->get_keyboard_shortcuts_enabled() && g_process_keydown_keyboard_shortcuts(wp))
            return 0;
        break;
    case WM_GETDLGCODE:
        break;
    case WM_CONTEXTMENU:
        if (wnd == reinterpret_cast<HWND>(wp)) {
            enum {
                ID_COPY = 1,
                ID_CLEAR,
                ID_EDGE_STYLE_NONE,
                ID_EDGE_STYLE_SUNKEN,
                ID_EDGE_STYLE_GREY,
                ID_HIDE_TRAILING_NEWLINE
            };

            POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            if (pt.x == -1 && pt.y == -1) {
                RECT rc;
                GetRelativeRect(wnd, HWND_DESKTOP, &rc);
                pt.x = rc.left + (rc.right - rc.left) / 2;
                pt.y = rc.top + (rc.bottom - rc.top) / 2;
            }

            uih::Menu menu;

            menu.append_command(L"&Copy", ID_COPY);
            menu.append_separator();
            menu.append_command(L"&Clear", ID_CLEAR);
            menu.append_separator();

            uih::Menu edge_style_submenu;
            edge_style_submenu.append_command(
                L"&None", ID_EDGE_STYLE_NONE, m_edge_style == EdgeStyle::None ? uih::Menu::flag_radiochecked : 0);
            edge_style_submenu.append_command(
                L"&Sunken", ID_EDGE_STYLE_SUNKEN, m_edge_style == EdgeStyle::Sunken ? uih::Menu::flag_radiochecked : 0);
            edge_style_submenu.append_command(
                L"&Grey", ID_EDGE_STYLE_GREY, m_edge_style == EdgeStyle::Grey ? uih::Menu::flag_radiochecked : 0);

            menu.append_submenu(L"&Edge style", edge_style_submenu.detach());
            menu.append_command(L"&Hide trailing newline", ID_HIDE_TRAILING_NEWLINE,
                m_hide_trailing_newline ? uih::Menu::flag_checked : 0);

            const int cmd = menu.run(wnd, pt);

            switch (cmd) {
            case ID_COPY:
                copy();
                break;
            case ID_CLEAR:
                s_clear();
                break;
            case ID_EDGE_STYLE_NONE:
                set_edge_style(EdgeStyle::None);
                break;
            case ID_EDGE_STYLE_SUNKEN:
                set_edge_style(EdgeStyle::Sunken);
                break;
            case ID_EDGE_STYLE_GREY:
                set_edge_style(EdgeStyle::Grey);
                break;
            case ID_HIDE_TRAILING_NEWLINE:
                set_hide_trailing_newline(!m_hide_trailing_newline);
                break;
            }
            return 0;
        }
        break;
    }
    return CallWindowProc(m_editproc, wnd, msg, wp, lp);
}

void ConsoleWindow::set_window_theme() const
{
    if (!m_wnd_edit)
        return;

    SetWindowTheme(m_wnd_edit, cui::colours::is_dark_mode_active() ? L"DarkMode_Explorer" : nullptr, nullptr);
}

static ui_extension::window_factory<ConsoleWindow> console_window_factory;

class ConsoleReceiver : public console_receiver {
    /**
     * We assume that this function may be called from any thread.
     *
     * However, in most callbacks you would want to use, you can assume calls
     * come from the main thread.
     *
     * Check the documentation of the callback to find out if this is true for
     * the callback you wish to use.
     */
    void print(const char* p_message, unsigned p_message_length) override
    {
        ConsoleWindow::s_on_message_received(p_message, p_message_length);
    }
};

static service_factory_single_t<ConsoleReceiver> console_console_receiver;

class ConsoleFontClient : public cui::fonts::client {
public:
    const GUID& get_client_guid() const override { return console_font_id; }

    void get_name(pfc::string_base& p_out) const override { p_out = "Console"; }

    cui::fonts::font_type_t get_default_font_type() const override { return cui::fonts::font_type_labels; }

    void on_font_changed() const override { ConsoleWindow::s_update_all_fonts(); }
};

class ConsoleColourClient : public cui::colours::client {
public:
    const GUID& get_client_guid() const override { return console_colours_client_id; }
    void get_name(pfc::string_base& p_out) const override { p_out = "Console"; }

    t_size get_supported_colours() const override
    {
        return cui::colours::colour_flag_background | cui::colours::colour_flag_text;
    }

    t_size get_supported_bools() const override { return cui::colours::bool_flag_dark_mode_enabled; }

    bool get_themes_supported() const override { return false; }

    void on_bool_changed(t_size mask) const override
    {
        if (mask & cui::colours::bool_flag_dark_mode_enabled) {
            ConsoleWindow::s_update_window_themes();
        }
    }
    void on_colour_changed(t_size mask) const override { ConsoleWindow::s_update_colours(); }
};

static ConsoleFontClient::factory<ConsoleFontClient> console_font_client;
static ConsoleColourClient::factory<ConsoleColourClient> console_colour_client;
