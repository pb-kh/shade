#pragma once

// Shade plugin SDK — core types.
//
// A plugin is a Qt plugin (shared module) that implements shade::Plugin and
// embeds a metadata JSON (see docs/plugin-api.md). Everything between the
// host and plugins (and between plugins) travels as shade::Signal over the
// bus. Plugins only include headers from <shade/...> plus Qt.

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <QtPlugin>

#include <functional>

#if defined(SHADE_SDK_BUILD)
#define SHADE_SDK_EXPORT Q_DECL_EXPORT
#else
#define SHADE_SDK_EXPORT Q_DECL_IMPORT
#endif

#define SHADE_API_MAJOR 1
#define SHADE_API_MINOR 1

namespace shade
{

// Reserved destinations.
inline constexpr const char *kHost = "shade";
inline constexpr const char *kBroadcast = "*";

// Built-in signal names.
namespace sig
{
// shade -> plugin
inline constexpr const char *WidgetRequest = "widget_request"; // {window}
inline constexpr const char *Activate = "activate";            // {window}
inline constexpr const char *Deactivate = "deactivate";        // {}
inline constexpr const char *PeekDismissed = "peek_dismissed"; // {id, reason}
inline constexpr const char *TerminalClosed = "terminal_closed"; // {reason, exit_code?, error?} (API 1.1)
// plugin -> shade
inline constexpr const char *Widget = "widget";             // {window, widget: QObject*}
inline constexpr const char *Peek = "peek";                 // see PeekRequest
inline constexpr const char *DismissPeek = "dismiss_peek";  // {id} (empty = all)
inline constexpr const char *ActivateWidget = "activate_widget"; // {window?}
// Embedded virtual terminal (API 1.1): {command, width, height} in pixels,
// signal.window optional (default: the window the pointer was last on). One
// terminal per (plugin, window); a new request replaces it.
inline constexpr const char *TerminalRequest = "terminal_request";
inline constexpr const char *TerminalClose = "terminal_close"; // {} (signal.window optional: all windows)
} // namespace sig

// A message on the bus. `source` is stamped by the bus endpoint: a plugin
// cannot forge another plugin's name. `window` optionally targets a monitor
// (the screen name); empty means "all windows".
struct Signal
{
    QString name;
    QString source;
    QString destination;
    QString window;
    QVariantMap payload;
};

// One endpoint on the bus, bound to a fixed source name. Delivery is always
// asynchronous (queued on the UI loop); send() never blocks.
class SHADE_SDK_EXPORT Bus
{
public:
    using Handler = std::function<void(const Signal &)>;

    virtual ~Bus();

    // Queue `signal` for delivery to signal.destination. Returns false (and
    // logs a diagnostic) when the signal is malformed or the destination is
    // unknown; nothing is delivered in that case.
    virtual bool send(Signal signal) = 0;

    // Register a handler for signals named `name` addressed to this endpoint.
    virtual int handle(const QString &name, Handler handler) = 0;
    virtual void unhandle(int handlerId) = 0;

    // Convenience.
    bool send(const QString &destination,
              const QString &name,
              const QVariantMap &payload = {},
              const QString &window = {})
    {
        return send(Signal{name, {}, destination, window, payload});
    }
};

// What the host hands a plugin at load time.
class SHADE_SDK_EXPORT Host
{
public:
    virtual ~Host();

    // The plugin's declared name (from its metadata).
    virtual QString pluginName() const = 0;
    // The plugin's endpoint on the bus.
    virtual Bus *bus() = 0;
    // The ["plugin:<name>"] section of config.toml as plain Qt values. Empty
    // when absent.
    virtual QVariantMap config() const = 0;
};

// Peek priorities (higher wins the front of the queue).
enum class PeekPriority { Low = 1, Medium = 2, High = 3 };

// How a presented peek turns into the full widget.
//  "hover": hovering the peek expands to the plugin's widget (e.g. media).
//  "top":   hovering does nothing; pushing the cursor to the screen top
//           expands (e.g. notifications).
//  "none":  never expands by itself (only via activate_widget).
inline constexpr const char *kExpandHover = "hover";
inline constexpr const char *kExpandTop = "top";
inline constexpr const char *kExpandNone = "none";

// Helper to build a `peek` signal payload.
// Splits a command line into argv with POSIX shell quoting rules: 'single'
// and "double" quotes, backslash escapes; no expansion, globbing or
// operators (the program is executed directly, not through a shell).
// Returns false on an unterminated quote or trailing backslash.
SHADE_SDK_EXPORT bool splitCommand(const QString &command, QStringList *argv);

inline QVariantMap peekPayload(const QString &id,
                               const QVariantMap &data,
                               PeekPriority priority = PeekPriority::Medium,
                               const QString &expand = kExpandHover,
                               int timeoutMs = -1 /* -1: host default, 0: never */)
{
    return QVariantMap{
        {QStringLiteral("id"), id},
        {QStringLiteral("data"), data},
        {QStringLiteral("priority"), static_cast<int>(priority)},
        {QStringLiteral("expand"), expand},
        {QStringLiteral("timeout"), timeoutMs},
    };
}

// The interface every plugin implements.
class Plugin
{
public:
    virtual ~Plugin() = default;
    // Called once after the module is loaded. Register bus handlers here.
    // Returning false unloads the plugin.
    virtual bool load(Host *host) = 0;
    // Called before the module is unloaded. Release everything.
    virtual void unload() {}
};

} // namespace shade

#define ShadePlugin_iid "org.shade.Plugin/1"
Q_DECLARE_INTERFACE(shade::Plugin, ShadePlugin_iid)
