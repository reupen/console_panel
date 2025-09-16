/**
 * \file main.cpp
 *
 * \brief Console panel component
 *
 * This component is an example of a multiple instance panel that takes
 * keyboard input
 *
 * It demonstrates the following relevant techniques:
 * - Subclassing the child control (using a helper function from another
 *   library) to process keyboard shortcuts
 * - Setting the font and colours of the child window
 * - Keeping a list of active windows and updating them from a callback
 *   (in this case designed such that the callback may come from any thread)
 */

#include "main.h"

using namespace std::string_view_literals;
using namespace std::chrono_literals;

/** Declare some component information */
DECLARE_COMPONENT_VERSION("Console panel", console_panel::version,
    "compiled: " COMPILATION_DATE "\n"
    "with Panel API version: " UI_EXTENSION_VERSION

);

constexpr auto IDC_EDIT = 1001;
constexpr auto MSG_UPDATE = WM_USER + 2;
constexpr auto ID_TIMER = 667;

/** \brief The maximum number of message we cache/display */
constexpr t_size maximum_messages = 200;

cfg_int cfg_last_edge_style(GUID{0x05550547, 0xbf98, 0x088c, 0xbe, 0x0e, 0x24, 0x95, 0xe4, 0x9b, 0x88, 0xc7},
    static_cast<int>(EdgeStyle::None));

cfg_bool cfg_last_hide_trailing_newline(
    {0x5db0b4d6, 0xf429, 0x4fc5, 0xb9, 0x1d, 0x29, 0x8e, 0xf3, 0x34, 0x75, 0x16}, false);

constexpr GUID console_font_id = {0x26059feb, 0x488b, 0x4ce1, {0x82, 0x4e, 0x4d, 0xf1, 0x13, 0xb4, 0x55, 0x8e}};

constexpr GUID console_colours_client_id
    = {0x9d814898, 0x0db4, 0x4591, {0xa7, 0xaa, 0x4e, 0x94, 0xdd, 0x07, 0xb3, 0x87}};

constexpr auto current_config_version = 0;

void ConsoleWindow::s_update_all_fonts()
{
    const auto old_font = std::move(s_font);
    s_font.reset(cui::fonts::helper(console_font_id).get_font());

    for (auto&& window : s_windows) {
        if (const HWND wnd = window->m_wnd_edit) {
            SetWindowFont(wnd, s_font.get(), TRUE);
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

void ConsoleWindow::s_on_message_received(std::string_view text)
{
    std::scoped_lock _(s_mutex);

    size_t offset{};
    std::string fixed_text;

    while (true) {
        const size_t index = text.find_first_of("\r\n"sv, offset);

        const auto fragment_length = index == std::string_view::npos ? std::string_view::npos : index - offset;
        const auto fragment = text.substr(offset, fragment_length);

        fixed_text.append(fragment);

        if (index == std::string_view::npos)
            break;

        offset = text.find_first_not_of('\r', index);

        if (offset == std::string_view::npos)
            break;

        if (text[offset] == '\n') {
            fixed_text.append("\r\n"sv);
            ++offset;
        }
    }

    const auto trim_pos = fixed_text.find_last_not_of("\r\n"sv);

    if (trim_pos == std::string::npos)
        return;

    fixed_text.resize(trim_pos + 1);

    s_messages.emplace_back(fixed_text);

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
    std::scoped_lock _(s_mutex);

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
    p_hook.add_node(
        new uie::simple_command_menu_node("Hide trailing newline", "Toggles visibility of the trailing newline.",
            get_hide_trailing_newline() ? uie::menu_node_t::state_checked : 0,
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
    std::scoped_lock _(s_mutex);
    std::string buffer;
    buffer.reserve(1024);

    for (auto iter = s_messages.begin(); iter != s_messages.end(); ++iter) {
        std::format_to(std::back_inserter(buffer), "[{:%H:%M:%S}] {}",
            std::chrono::time_point_cast<std::chrono::seconds>(iter->m_timestamp), iter->m_message);

        if (!m_hide_trailing_newline || std::next(iter) != s_messages.end()) {
            std::format_to(std::back_inserter(buffer), "\r\n");
        }
    }

    uSetWindowText(m_wnd_edit, buffer.c_str());
    const int len = Edit_GetLineCount(m_wnd_edit);
    Edit_Scroll(m_wnd_edit, len, 0);
    m_last_update_time_point = std::chrono::steady_clock::now();
}

void ConsoleWindow::update_content_throttled() noexcept
{
    if (m_timer_active)
        return;

    const auto now = std::chrono::steady_clock::now();
    const auto time_since_last_update = now - m_last_update_time_point;

    if (time_since_last_update < 250ms) {
        const auto ms_since_last_update
            = std::chrono::duration_cast<std::chrono::milliseconds>((now - m_last_update_time_point)).count();
        SetTimer(get_wnd(), ID_TIMER, 250 - gsl::narrow<uint32_t>(ms_since_last_update), nullptr);
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
            std::scoped_lock _(s_mutex);
            /** Store a window handle in this list, used in global notifications (in any thread) which
             * updates the panels */
            s_notify_list.emplace_back(wnd);
        }

        const auto edit_ex_styles = get_edit_ex_styles();

        /** Create our edit window */
        m_wnd_edit = CreateWindowEx(edit_ex_styles, WC_EDIT, _T(""),
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOVSCROLL | WS_VSCROLL | ES_READONLY | ES_MULTILINE, 0, 0, 0, 0,
            wnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_EDIT)), core_api::get_my_instance(), nullptr);

        if (m_wnd_edit) {
            set_window_theme();

            if (s_font) {
                SetWindowFont(m_wnd_edit, s_font.get(), FALSE);
            } else {
                /** First window - create the font handle */
                s_update_all_fonts();
            }

            if (!s_background_brush)
                s_update_colours();

            uih::subclass_window(
                m_wnd_edit, [this](auto wnd_proc, auto wnd, auto msg, auto wp, auto lp) -> std::optional<LRESULT> {
                    return handle_edit_message(wnd_proc, wnd, msg, wp, lp);
                });

            SendMessage(wnd, MSG_UPDATE, 0, 0);
        }
        break;
    }
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
    case WM_CLOSE:
        return 0;
    case WM_DESTROY:
        m_wnd_edit = nullptr;
        std::erase(s_windows, this);

        {
            std::scoped_lock _(s_mutex);
            std::erase(s_notify_list, wnd);
        }
        break;
    case WM_NCDESTROY:
        if (s_windows.empty()) {
            s_font.reset();
            s_background_brush.reset();
        }
        break;
    }
    return DefWindowProc(wnd, msg, wp, lp);
}

std::optional<LRESULT> ConsoleWindow::handle_edit_message(WNDPROC wnd_proc, HWND wnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (const auto result = uih::handle_subclassed_window_buffered_painting(wnd_proc, wnd, msg, wp, lp))
        return result;

    switch (msg) {
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
    case WM_CONTEXTMENU: {
        if (wnd != reinterpret_cast<HWND>(wp))
            break;

        POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        const auto from_keyboard = pt.x == -1 && pt.y == -1;

        if (!from_keyboard && SendMessage(m_wnd_edit, WM_NCHITTEST, 0, lp) != HTCLIENT)
            break;

        if (from_keyboard) {
            RECT rc;
            GetRelativeRect(wnd, HWND_DESKTOP, &rc);
            pt.x = rc.left + (rc.right - rc.left) / 2;
            pt.y = rc.top + (rc.bottom - rc.top) / 2;
        }

        const uih::Menu menu;
        uih::MenuCommandCollector command_collector;

        menu.append_command(command_collector.add([this] { copy(); }), L"Copy");
        menu.append_separator();
        menu.append_command(command_collector.add([] { s_clear(); }), L"Clear");
        menu.append_separator();

        uih::Menu edge_style_submenu;
        edge_style_submenu.append_command(command_collector.add([this] { set_edge_style(EdgeStyle::None); }), L"None",
            {.is_radio_checked = m_edge_style == EdgeStyle::None});
        edge_style_submenu.append_command(command_collector.add([this] { set_edge_style(EdgeStyle::Sunken); }),
            L"Sunken", {.is_radio_checked = m_edge_style == EdgeStyle::Sunken});
        edge_style_submenu.append_command(command_collector.add([this] { set_edge_style(EdgeStyle::Grey); }), L"Grey",
            {.is_radio_checked = m_edge_style == EdgeStyle::Grey});

        menu.append_submenu(std::move(edge_style_submenu), L"Edge style");
        menu.append_command(command_collector.add([this] { set_hide_trailing_newline(!m_hide_trailing_newline); }),
            L"Hide trailing newline", {.is_checked = m_hide_trailing_newline});

        menu_helpers::win32_auto_mnemonics(menu.get());

        const auto cmd = menu.run(wnd, pt);

        command_collector.execute(cmd);

        return 0;
    }
    }
    return {};
}

void ConsoleWindow::set_window_theme() const
{
    if (!m_wnd_edit)
        return;

    SetWindowTheme(m_wnd_edit, cui::colours::is_dark_mode_active() ? L"DarkMode_Explorer" : nullptr, nullptr);
}

static uie::window_factory<ConsoleWindow> console_window_factory;

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
    void print(const char* p_message, size_t p_message_length) override
    {
        ConsoleWindow::s_on_message_received({p_message, strnlen_s(p_message, p_message_length)});
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

    uint32_t get_supported_colours() const override
    {
        return cui::colours::colour_flag_background | cui::colours::colour_flag_text;
    }

    uint32_t get_supported_bools() const override { return cui::colours::bool_flag_dark_mode_enabled; }

    bool get_themes_supported() const override { return false; }

    void on_bool_changed(uint32_t mask) const override
    {
        if (mask & cui::colours::bool_flag_dark_mode_enabled) {
            ConsoleWindow::s_update_window_themes();
        }
    }
    void on_colour_changed(uint32_t mask) const override { ConsoleWindow::s_update_colours(); }
};

static ConsoleFontClient::factory<ConsoleFontClient> console_font_client;
static ConsoleColourClient::factory<ConsoleColourClient> console_colour_client;
