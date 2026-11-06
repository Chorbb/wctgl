# TGL Port — оболочка Godot

Godot-проект, который показывает игру из ядра порта (`native/tglcore.dll`).

## Запуск

1. Нужны Godot 4.x **.NET** (Mono) и .NET SDK 8 или новее (`dotnet --list-sdks`).
2. В Godot: **Import** → выбрать `project.godot` в этой папке → **Import & Edit**.
3. Нажать **F5** (Run). При первом запуске Godot соберёт C#-проект.
4. ROM ищется по пути `C:/TGL_claude/rom/Guardian Legend, The (USA).nes`;
   если его там нет, появится окно выбора файла, путь запомнится.

## Управление

| NES | Клавиатура | Геймпад |
|---|---|---|
| Крестовина | Стрелки / WASD | Крестовина, левый стик |
| B (огонь) | Z | нижняя кнопка |
| A (доп. оружие) | X | правая кнопка |
| Start | Enter | Start |
| Select | Shift | Back / Select |
| Перезагрузка | F5 | — |
| Выход | Esc | — |

`native/tglcore.dll` собирается из `core/` командой `make dll` (clang + lld-link), ROM в проект не входит.
