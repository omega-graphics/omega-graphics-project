#include <OmegaGTE.h>
#include <gtk/gtk.h>

OmegaGTE::GTE gte;

static void start_application(GtkApplication *app,gpointer user_data){
    GtkWidget *window;

    window = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(window),"2DTest");
    gtk_widget_show_all(window);
    
};

int main(int argc,char *argv[]){
    
    gte = OmegaGTE::InitWithDefaultDevice();
    

    GtkApplication *app = gtk_application_new("org.omegagraphics.OmegaGTETest",G_APPLICATION_FLAGS_NONE);
    g_signal_connect(app,"activate",G_CALLBACK(start_application),NULL);
    auto status = g_application_run(G_APPLICATION(app),argc,argv);
    g_object_unref(app);

    OmegaGTE::Close(gte);

    return status;
};