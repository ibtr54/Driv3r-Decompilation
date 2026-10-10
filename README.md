# Driver 3 — Decompilation

Reverse engineering and decompilation of *Driver 3 (Driv3r)*, focused on analyzing its internal structure and reconstructing its code.

To run this, you need the game files.
Provide the game path. On Visual Studio go to Project-><Project Name> Properties. Then click on the debug tab. Fill in the arguments in the textbox labeled Command line arguments.

Settings accepted in the options screens are saved to the game's own profile (`Saves\D3P_<name>\ProfileData`, the most recently saved one), so the original game sees them too. Before the first write a copy is kept as `ProfileData.bak`. If there is no profile yet (create one by starting the original game once), settings are not saved.

## Documentation

Soon a LaTeX document will be included in the repository, providing a detailed explanation of the decompilation process, methodology, and technical findings.

## Objectives

* Analyze the game's executable and internal functions.
* Investigate control flow and data structures.
* Document the decompilation and code reconstruction process.

## Disclaimer

This project is intended for educational and research purposes. All intellectual property remains with its respective owners.

