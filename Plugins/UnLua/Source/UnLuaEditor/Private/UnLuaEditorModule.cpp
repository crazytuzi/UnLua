// Tencent is pleased to support the open source community by making UnLua available.
// 
// Copyright (C) 2019 THL A29 Limited, a Tencent company. All rights reserved.
//
// Licensed under the MIT License (the "License"); 
// you may not use this file except in compliance with the License. You may obtain a copy of the License at
//
// http://opensource.org/licenses/MIT
//
// Unless required by applicable law or agreed to in writing, 
// software distributed under the License is distributed on an "AS IS" BASIS, 
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. 
// See the License for the specific language governing permissions and limitations under the License.

#include "Compat/UObjectHash.h"
#include "UnLuaEditorStyle.h"
#include "UnLuaEditorCommands.h"
#include "Misc/CoreDelegates.h"
#include "Editor.h"
#include "BlueprintEditorModule.h"
#include "EditorHotReloadWatcher.h"
#include "UnLuaIntelliSenseGenerator.h"
#include "UnLua.h"
#include "ISettingsModule.h"
#include "ISettingsSection.h"
#include "LuaEnvLocator.h"
#include "UnLuaEditorSettings.h"
#include "UnLuaEditorFunctionLibrary.h"
#include "UnLuaFunctionLibrary.h"
#include "UnLuaInterface.h"
#include "Kismet2/DebuggerCommands.h"
#include "Interfaces/IMainFrameModule.h"
#include "Interfaces/IPluginManager.h"
#include "Settings/ProjectPackagingSettings.h"
#include "Toolbars/AnimationBlueprintToolbar.h"
#include "Toolbars/BlueprintToolbar.h"
#include "Toolbars/MainMenuToolbar.h"
#if ENGINE_MAJOR_VERSION > 4
#include "UObject/ObjectSaveContext.h"
#endif

#define LOCTEXT_NAMESPACE "FUnLuaEditorModule"

namespace UnLua
{
    class FUnLuaEditorModule : public IModuleInterface
    {
    public:
        FUnLuaEditorModule()
        {
        }

        virtual void StartupModule() override
        {
            Style = FUnLuaEditorStyle::GetInstance();

            FUnLuaEditorCommands::Register();

            FCoreDelegates::OnPostEngineInit.AddRaw(this, &FUnLuaEditorModule::OnPostEngineInit);

            MainMenuToolbar = MakeShareable(new FMainMenuToolbar);
            BlueprintToolbar = MakeShareable(new FBlueprintToolbar);
            AnimationBlueprintToolbar = MakeShareable(new FAnimationBlueprintToolbar);

            UUnLuaEditorFunctionLibrary::WatchScriptDirectory();

#if ENGINE_MAJOR_VERSION > 4
            UPackage::PreSavePackageWithContextEvent.AddRaw(this, &FUnLuaEditorModule::OnPackageSavingWithContext);
            UPackage::PackageSavedWithContextEvent.AddRaw(this, &FUnLuaEditorModule::OnPackageSavedWithContext);
#else
            UPackage::PreSavePackageEvent.AddRaw(this, &FUnLuaEditorModule::OnPackageSaving);
            UPackage::PackageSavedEvent.AddRaw(this, &FUnLuaEditorModule::OnPackageSaved);
#endif
            SetupPackagingSettings();

            ULuaEnvLocator::OnEditorLocate.BindRaw(this, &FUnLuaEditorModule::OnEditorLocate);
        }

        virtual void ShutdownModule() override
        {
            if (GEditor)
            {
                GEditor->OnBlueprintPreCompile().RemoveAll(this);
                GEditor->OnBlueprintCompiled().RemoveAll(this);
            }

            FUnLuaEditorCommands::Unregister();
            FCoreDelegates::OnPostEngineInit.RemoveAll(this);
            UnregisterSettings();

#if ENGINE_MAJOR_VERSION > 4
            UPackage::PreSavePackageWithContextEvent.RemoveAll(this);
            UPackage::PackageSavedWithContextEvent.RemoveAll(this);
#else
            UPackage::PreSavePackageEvent.RemoveAll(this);
            UPackage::PackageSavedEvent.RemoveAll(this);
#endif

            ULuaEnvLocator::OnEditorLocate.Unbind();
        }

    private:
        void OnPostEngineInit()
        {
            RegisterSettings();

            if (!GEditor)
            {
                // Loading MainFrame module with '-game' is not supported
                return;
            }

            GEditor->OnBlueprintPreCompile().AddRaw(this, &FUnLuaEditorModule::OnBlueprintPreCompile);

            MainMenuToolbar->Initialize();
            BlueprintToolbar->Initialize();
            AnimationBlueprintToolbar->Initialize();
            FUnLuaIntelliSenseGenerator::Get()->Initialize();

            IMainFrameModule& MainFrameModule = FModuleManager::LoadModuleChecked<IMainFrameModule>(TEXT("MainFrame"));
            MainFrameModule.OnMainFrameCreationFinished().AddRaw(this, &FUnLuaEditorModule::OnMainFrameCreationFinished);

            Env = MakeUnique<FLuaEnv>();
            Env->Watch<FEditorHotReloadWatcher>({
                IPluginManager::Get().FindPlugin(TEXT("UnLua"))->GetContentDir() / "Script",
                FPaths::ProjectContentDir() / "Script"
            });
            Env->Start("Editor.Main", {});
        }

        void OnMainFrameCreationFinished(TSharedPtr<SWindow> InRootWindow, bool bIsNewProjectWindow)
        {
            // register default key input to 'HotReload' Lua
            FPlayWorldCommands::GlobalPlayWorldActions->MapAction(
                FUnLuaEditorCommands::Get().HotReload, FExecuteAction::CreateStatic(UUnLuaFunctionLibrary::HotReload), FCanExecuteAction());
        }

        void RegisterSettings() const
        {
            ISettingsModule* SettingsModule = FModuleManager::GetModulePtr<ISettingsModule>("Settings");
            if (SettingsModule)
            {
                const TSharedPtr<ISettingsSection> Section = SettingsModule->RegisterSettings("Project", "Plugins", "UnLua Editor",
                                                                                              LOCTEXT("UnLuaEditorSettingsName", "UnLua Editor"),
                                                                                              LOCTEXT("UnLuaEditorSettingsDescription", "UnLua Editor Settings"),
                                                                                              GetMutableDefault<UUnLuaEditorSettings>());
                Section->OnModified().BindRaw(this, &FUnLuaEditorModule::OnSettingsModified);
            }
        }

        void UnregisterSettings() const
        {
            ISettingsModule* SettingsModule = FModuleManager::GetModulePtr<ISettingsModule>("Settings");
            if (SettingsModule)
                SettingsModule->UnregisterSettings("Project", "Plugins", "UnLua Editor");
        }

        bool OnSettingsModified() const
        {
            const auto BuildFile = IPluginManager::Get().FindPlugin(TEXT("UnLua"))->GetBaseDir() / "Source/UnLua/UnLua.Build.cs";
            auto& FileManager = IFileManager::Get();
            if (FileManager.FileExists(*BuildFile))
                FileManager.SetTimeStamp(*BuildFile, FDateTime::UtcNow());
            return true;
        }

#if ENGINE_MAJOR_VERSION > 4
        void OnPackageSavingWithContext(UPackage* Package, FObjectPreSaveContext Context)
        {
            OnPackageSaving(Package);
        }

        void OnPackageSavedWithContext(const FString& PackageFileName, UPackage* Package, FObjectPostSaveContext Context)
        {
            OnPackageSaved(PackageFileName, Package);
        }
#endif

        void OnPackageSaving(UPackage* Package)
        {
            if (!GEditor || !GEditor->PlayWorld)
                return;

            ForEachObjectWithPackage(Package, [this, Package](UObject* Object)
            {
                const auto Class = Cast<UClass>(Object);
                if (!Class || Class->GetName().StartsWith(TEXT("SKEL_")) || Class->GetName().StartsWith(TEXT("REINST_")))
                    return true;
                SuspendedPackages.Add(Package, Class);
                return false;
            }, false);

            for (const auto Pair : SuspendedPackages)
                ULuaFunction::SuspendOverrides(Pair.Value);
        }

        void OnPackageSaved(const FString& PackageFileName, UObject* Outer)
        {
            if (!GEditor || !GEditor->PlayWorld)
                return;

            UPackage* Package = (UPackage*)Outer;
            if (SuspendedPackages.Contains(Package))
            {
                ULuaFunction::ResumeOverrides(SuspendedPackages[Package]);
                SuspendedPackages.Remove(Package);
            }
        }

        void SetupPackagingSettings()
        {
            static auto ScriptPaths = {TEXT("Script"), TEXT("../Plugins/UnLua/Content/Script")};
            const auto PackagingSettings = GetMutableDefault<UProjectPackagingSettings>();
            bool bModified = false;
            auto Exists = [&](const auto Path)
            {
                for (const auto& DirPath : PackagingSettings->DirectoriesToAlwaysStageAsUFS)
                {
                    if (DirPath.Path == Path)
                        return true;
                }
                return false;
            };

            for (auto& ScriptPath : ScriptPaths)
            {
                if (Exists(ScriptPath))
                    continue;

                FDirectoryPath DirectoryPath;
                DirectoryPath.Path = ScriptPath;
                PackagingSettings->DirectoriesToAlwaysStageAsUFS.Add(DirectoryPath);
                bModified = true;
            }

            if (bModified)
            {
#if ENGINE_MAJOR_VERSION >= 5
                PackagingSettings->TryUpdateDefaultConfigFile();
#else
                PackagingSettings->UpdateDefaultConfigFile();
#endif
            }
        }

        void OnBlueprintPreCompile(UBlueprint* Blueprint)
        {
            auto Handle = Blueprint->OnCompiled().AddRaw(this, &FUnLuaEditorModule::OnBlueprintCompiled);
            ObservingBlueprints.Add(Blueprint, Handle);
        }

        void OnBlueprintCompiled(UBlueprint* Blueprint)
        {
            ObservingBlueprints.Remove(Blueprint);
            auto Class = Blueprint->GeneratedClass;
            if (!Class->ImplementsInterface(UUnLuaInterface::StaticClass()))
                return;
            if (!IUnLuaInterface::Execute_RunInEditor(Class->GetDefaultObject()))
                return;
            Env->TryBind(Blueprint->GeneratedClass);
        }

        FLuaEnv* OnEditorLocate(const UObject* Object) const
        {
            const UObject* CDO;
            if (Object->HasAnyFlags(RF_ClassDefaultObject | RF_ArchetypeObject))
            {
                CDO = Object;
            }
            else
            {
                const auto Class = Cast<UClass>(Object);
                CDO = Class ? Class->GetDefaultObject(false) : Object->GetClass()->GetDefaultObject(false);
            }

            if (!CDO)
                return nullptr;

            if (CDO->HasAnyFlags(RF_NeedInitialization))
                return nullptr;

            if (!CDO->GetClass()->ImplementsInterface(UUnLuaInterface::StaticClass()))
                return nullptr;

            if (!IUnLuaInterface::Execute_RunInEditor(CDO))
                return nullptr;

            return Env.Get();
        }

        TSharedPtr<FBlueprintToolbar> BlueprintToolbar;
        TSharedPtr<FAnimationBlueprintToolbar> AnimationBlueprintToolbar;
        TSharedPtr<FMainMenuToolbar> MainMenuToolbar;
        TSharedPtr<FUnLuaIntelliSenseGenerator> IntelliSenseGenerator;
        TSharedPtr<ISlateStyle> Style;
        TMap<UPackage*, UClass*> SuspendedPackages;
        TUniquePtr<FLuaEnv> Env;
        TMap<UBlueprint*, FDelegateHandle> ObservingBlueprints;
    };
}

IMPLEMENT_MODULE(UnLua::FUnLuaEditorModule, UnLuaEditor)

#undef LOCTEXT_NAMESPACE
