#include <juce_audio_utils/juce_audio_utils.h>
#include "MainComponent.h"

//==============================================================================
class AudioTranscriptApplication : public juce::JUCEApplication
{
public:
    AudioTranscriptApplication() = default;

    const juce::String getApplicationName() override       { return "Audio Transcript Editor"; }
    const juce::String getApplicationVersion() override    { return "1.0.0"; }
    bool moreThanOneInstanceAllowed() override              { return true; }

    void initialise(const juce::String& /*commandLine*/) override
    {
        mainWindow = std::make_unique<MainWindow>(getApplicationName(),
                                                   new MainComponent());
    }

    void shutdown() override { mainWindow = nullptr; }
    void systemRequestedQuit() override { quit(); }
    void anotherInstanceStarted(const juce::String&) override {}

    //==============================================================================
    class MainWindow : public juce::DocumentWindow,
                       private juce::MenuBarModel
    {
    public:
        MainWindow(const juce::String& name, MainComponent* component)
            : DocumentWindow(name, juce::Colour(0xFF1a1a2e), DocumentWindow::allButtons),
              mainComponent(component)
        {
            setContentOwned(component, true);
            setMenuBar(this);
            setResizable(true, true);
            centreWithSize(component->getWidth(), component->getHeight());
            setVisible(true);
        }

        void closeButtonPressed() override
        {
            JUCEApplication::getInstance()->systemRequestedQuit();
        }

    private:
        enum { File_Open = 1, File_Exit, Help_About };

        juce::StringArray getMenuBarNames() override
        {
            return { juce::String::fromUTF8("文件"),
                     juce::String::fromUTF8("帮助") };
        }

        juce::PopupMenu getMenuForIndex(int menuIndex, const juce::String&) override
        {
            juce::PopupMenu menu;

            if (menuIndex == 0)
            {
                menu.addItem(File_Open, juce::String::fromUTF8("打开音频..."), true, false);
                menu.addSeparator();
                menu.addItem(File_Exit, juce::String::fromUTF8("退出"));
            }
            else if (menuIndex == 1)
            {
                menu.addItem(Help_About, juce::String::fromUTF8("关于..."));
            }

            return menu;
        }

        void menuItemSelected(int menuItemID, int) override
        {
            switch (menuItemID)
            {
                case File_Open:  mainComponent->openAudioFile(); break;
                case File_Exit:  JUCEApplication::getInstance()->systemRequestedQuit(); break;
                case Help_About: mainComponent->aboutDialog(); break;
                default: break;
            }
        }

        MainComponent* mainComponent;
        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainWindow)
    };

private:
    std::unique_ptr<MainWindow> mainWindow;
};

START_JUCE_APPLICATION(AudioTranscriptApplication)
