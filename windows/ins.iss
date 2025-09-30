[Setup]
AppName=ZipCombiner
AppVersion=1.3
WizardStyle=modern
DefaultDirName={autopf}\ZipCombiner
DefaultGroupName=ZipCombiner
UninstallDisplayIcon={app}\ZipCombiner.exe
OutputDir=C:\Users\murim\OneDrive\Desktop

[Files]
Source: "ZipCombiner.exe"; DestDir: "{app}"
Source: "*.dll"; DestDir: "{app}"
Source: "platforms\*dll"; DestDir: "{app}\platforms"

[Icons]
Name: "{group}\ZipCombiner"; Filename: "{app}\ZipCombiner.exe"
