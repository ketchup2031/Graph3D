# MechForge

MechForge — проект параметрической CAD-системы для механического проектирования.
Стартовое окно Qt показывает неподвижную проекцию куба, построенного и проверенного
геометрическим ядром Open CASCADE Technology (OCCT). План следующих этапов —
в [дорожной карте](MechForge_Development_Plan.md).

## Сборка на Windows

Установите [MSYS2](https://www.msys2.org/) и откройте терминал **MSYS2 UCRT64**.
Обновите среду командой `pacman -Syu`. Если MSYS2 попросит закрыть терминал,
откройте его снова и повторите команду обновления.

```bash
pacman -S --needed mingw-w64-ucrt-x86_64-{gcc,cmake,ninja,qt6-base,opencascade}
```

В том же терминале перейдите в папку проекта. Например, для
`C:\Projects\MechForge` выполните `cd /c/Projects/MechForge`. Затем:

```bash
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
./build/dev/MechForge.exe
```

Запускайте программу из UCRT64, чтобы в пути поиска были DLL этой среды.
Пакеты Qt и OCCT должны быть из одной среды с компилятором. Для сборки нужны
C++20, CMake 3.24 или новее, Ninja, Qt 6.5–6.x и OCCT 7.x.

Проверка `preview_box` подтверждает, что OCCT построил валидное тело и что для
отрисовки переданы все 12 рёбер куба длиной 100 мм. Сборка на Linux пока не проверена.

Лицензия MechForge ещё не выбрана. Сведения о прямых зависимостях см.
в [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
