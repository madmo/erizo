#include <app.h>
#include <window.h>
#include <log.h>

static app_t* app_handle = NULL;
void platform_init(app_t* app, int argc, char** argv) {
    app_handle = app;
    if (argc == 2) {
        app_open(app, argv[1]);
    }
    if (app->instance_count == 0) {
        //  Construct a dummy window, which triggers GLFW initialization
        //  and may cause the application to open a file (if it was
        //  double-clicked or dragged onto the icon).
        window_new("", 1.0f, 1.0f);

        if (app->instance_count == 0) {
            app_open(app, ":/sphere");
        }
    }
}

void platform_window_bind(GLFWwindow* window) {

}

/*  Shows a warning dialog with the given text */
void platform_warning(const char* title, const char* text) {
    log_error(text);
}