// Main.cs — оболочка Godot для ядра порта The Guardian Legend.
//
// Ядро (эмулятор + переведённый код) живёт в native/tglcore.dll и вызывается через P/Invoke.
// Здесь только то, что относится к «экрану»: загрузка ROM, темп 60 кадров/с,
// вывод картинки 256x240 и чтение клавиатуры/геймпада.
//
// Управление:  стрелки/WASD — крестовина, Z — B (огонь), X — A (доп. оружие),
//              Enter — Start, Shift — Select, F5 — Reset, Esc — выход.
// Геймпад:     крестовина/левый стик, нижняя кнопка — B, правая — A, Start, Back/Select.

using Godot;
using System;
using System.Reflection;
using System.Runtime.InteropServices;

public partial class Main : Node2D
{
    const string Lib = "tglcore";

    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] static extern int tgl_api_version();
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] static extern int tgl_load_rom(byte[] rom, int size);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] static extern void tgl_reset();
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] static extern void tgl_run_frame(int buttons);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] static extern void tgl_get_frame_rgba(byte[] rgba);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] static extern int tgl_frame_count();

    // Где искать ROM по умолчанию. Если файла нет, откроется окно выбора, путь запомнится.
    const string DefaultRomPath = "C:/TGL_claude/rom/Guardian Legend, The (USA).nes";
    const string SettingsPath = "user://settings.cfg";

    const double NesFrameTime = 1.0 / 60.0988;   // частота кадров NES (NTSC)
    const int W = 256, H = 240;

    static bool _resolverSet;

    readonly byte[] _rgba = new byte[W * H * 4];
    Image _image;
    ImageTexture _texture;
    Sprite2D _screen;
    Label _message;
    bool _running;
    double _acc;

    public override void _Ready()
    {
        _screen = new Sprite2D { Centered = false };
        AddChild(_screen);
        _message = new Label { Position = new Vector2(8, 8), AutowrapMode = TextServer.AutowrapMode.WordSmart, Size = new Vector2(240, 220) };
        _message.AddThemeFontSizeOverride("font_size", 8);
        AddChild(_message);

        _image = Image.CreateEmpty(W, H, false, Image.Format.Rgba8);
        _texture = ImageTexture.CreateFromImage(_image);
        _screen.Texture = _texture;

        try
        {
            SetupNativeLibrary();
            int ver = tgl_api_version();
            if (ver != 1) { ShowMessage($"Неожиданная версия tglcore.dll: {ver}"); return; }
        }
        catch (Exception e)
        {
            ShowMessage("Не удалось загрузить native/tglcore.dll:\n" + e.Message);
            return;
        }

        string path = LoadSetting("rom_path", DefaultRomPath);
        if (!TryLoadRom(path)) AskForRom();
    }

    // DLL лежит в res://native/. Подсказываем .NET, где её искать.
    static void SetupNativeLibrary()
    {
        if (_resolverSet) return;
        _resolverSet = true;
        NativeLibrary.SetDllImportResolver(Assembly.GetExecutingAssembly(), (name, asm, searchPath) =>
        {
            if (name != Lib) return IntPtr.Zero;
            string local = ProjectSettings.GlobalizePath("res://native/tglcore.dll");
            if (NativeLibrary.TryLoad(local, out IntPtr h)) return h;
            string nextToExe = System.IO.Path.Combine(System.IO.Path.GetDirectoryName(OS.GetExecutablePath()) ?? "", "tglcore.dll");
            if (NativeLibrary.TryLoad(nextToExe, out h)) return h;
            return IntPtr.Zero;
        });
    }

    bool TryLoadRom(string path)
    {
        if (string.IsNullOrEmpty(path) || !System.IO.File.Exists(path)) return false;
        byte[] rom = System.IO.File.ReadAllBytes(path);
        int err = tgl_load_rom(rom, rom.Length);
        if (err != 0)
        {
            ShowMessage($"ROM не подошёл (код {err}). Нужен The Guardian Legend (USA), формат .nes.");
            return false;
        }
        SaveSetting("rom_path", path);
        ShowMessage("");
        _running = true;
        return true;
    }

    void AskForRom()
    {
        ShowMessage("Выберите файл ROM The Guardian Legend (.nes)");
        var dlg = new FileDialog
        {
            FileMode = FileDialog.FileModeEnum.OpenFile,
            Access = FileDialog.AccessEnum.Filesystem,
            Filters = new[] { "*.nes ; NES ROM" },
            Title = "ROM The Guardian Legend",
            Size = new Vector2I(700, 500),
        };
        dlg.FileSelected += p => { if (!TryLoadRom(p)) AskForRom(); };
        AddChild(dlg);
        dlg.PopupCentered();
    }

    public override void _Process(double delta)
    {
        if (Input.IsKeyPressed(Key.Escape)) GetTree().Quit();
        if (!_running) return;

        _acc += delta;
        int frames = 0;
        while (_acc >= NesFrameTime && frames < 4)   // не больше 4 кадров за раз, если Godot подтормозил
        {
            tgl_run_frame(ReadButtons());
            _acc -= NesFrameTime;
            frames++;
        }
        if (_acc > NesFrameTime * 4) _acc = 0;

        if (frames > 0)
        {
            tgl_get_frame_rgba(_rgba);
            _image.SetData(W, H, false, Image.Format.Rgba8, _rgba);
            _texture.Update(_image);
        }
    }

    public override void _UnhandledInput(InputEvent e)
    {
        if (e is InputEventKey k && k.Pressed && !k.Echo && k.Keycode == Key.F5 && _running) tgl_reset();
    }

    // Кнопки NES: A B Select Start Up Down Left Right = биты 0..7
    static int ReadButtons()
    {
        int b = 0;
        const int pad = 0;
        float sx = Input.GetJoyAxis(pad, JoyAxis.LeftX), sy = Input.GetJoyAxis(pad, JoyAxis.LeftY);

        if (Pressed(Key.X) || Input.IsJoyButtonPressed(pad, JoyButton.B)) b |= 0x01;            // A
        if (Pressed(Key.Z) || Input.IsJoyButtonPressed(pad, JoyButton.A)) b |= 0x02;            // B
        if (Pressed(Key.Shift) || Input.IsJoyButtonPressed(pad, JoyButton.Back)) b |= 0x04;     // Select
        if (Pressed(Key.Enter) || Input.IsJoyButtonPressed(pad, JoyButton.Start)) b |= 0x08;    // Start
        if (Pressed(Key.Up) || Pressed(Key.W) || Input.IsJoyButtonPressed(pad, JoyButton.DpadUp) || sy < -0.5f) b |= 0x10;
        if (Pressed(Key.Down) || Pressed(Key.S) || Input.IsJoyButtonPressed(pad, JoyButton.DpadDown) || sy > 0.5f) b |= 0x20;
        if (Pressed(Key.Left) || Pressed(Key.A) || Input.IsJoyButtonPressed(pad, JoyButton.DpadLeft) || sx < -0.5f) b |= 0x40;
        if (Pressed(Key.Right) || Pressed(Key.D) || Input.IsJoyButtonPressed(pad, JoyButton.DpadRight) || sx > 0.5f) b |= 0x80;

        // на крестовине NES нельзя нажать противоположные стороны одновременно
        if ((b & 0x30) == 0x30) b &= ~0x30;
        if ((b & 0xC0) == 0xC0) b &= ~0xC0;
        return b;
    }

    static bool Pressed(Key k) => Input.IsPhysicalKeyPressed(k);

    void ShowMessage(string text)
    {
        _message.Text = text;
        _message.Visible = text.Length > 0;
    }

    static string LoadSetting(string key, string fallback)
    {
        var cfg = new ConfigFile();
        if (cfg.Load(SettingsPath) != Error.Ok) return fallback;
        return cfg.GetValue("main", key, fallback).AsString();
    }

    static void SaveSetting(string key, string value)
    {
        var cfg = new ConfigFile();
        cfg.Load(SettingsPath);
        cfg.SetValue("main", key, value);
        cfg.Save(SettingsPath);
    }
}
