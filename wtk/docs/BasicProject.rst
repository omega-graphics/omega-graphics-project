========================
How to Setup A Basic App
========================


.. code-block:: cpp


    #include <OmegaWTK.h>
    
    using namespace OmegaWTK;

    int omegaWTKMain(OmegaWTK::AppInst *app){
        
        SharedHandle<AppWindow> window = make<AppWindow>(OmegaWTK::Rect(0,0,1000,1000));
       

        SharedHandle<Button> button = make<UI::Button>(Rect(0,0,200,100));

        window->addWidget(button);

        app->windowManager->setRootWindow(window);
        app->windowManager->displayRootWindow();
        return 0;
    };

