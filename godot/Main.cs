// Main.cs — оболочка Godot для ядра порта The Guardian Legend.
//
// Ядро (эмулятор + переведённый код) живёт в native/tglcore.dll и вызывается через P/Invoke.
// Здесь только то, что относится к «экрану»: загрузка ROM, темп 60 кадров/с,
// вывод картинки 256x240 и чтение клавиатуры/геймпада.
//
// Управление:  стрелки/WASD — крестовина, Z — B (огонь), X — A (доп. оружие),
//              Enter — Start, Shift — Select, F5 — Reset, M — звук вкл/выкл,
//              F7 — перечитать моды и начать заново, Esc — выход.
// Моды: файлы *.tglpatch в папке mods рядом с проектом (собираются tools/mod.py из .tglmod).
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
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] static extern void tgl_audio_set_rate(double hz);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] static extern int tgl_audio_read(short[] buf, int max);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] static extern int tgl_patch_rom(int bank, int addr, byte[] data, int len);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] static extern void tgl_use_translation(int on);

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

    // звук: ядро отдаёт моно 16 бит, Godot проигрывает через AudioStreamGenerator
    AudioStreamPlayer _player;
    AudioStreamGeneratorPlayback _playback;
    readonly short[] _abuf = new short[8192];
    Vector2[] _aframes = new Vector2[8192];
    float _mixRate;
    int _capacity;
    bool _muted;

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
            if (ver != 3) { ShowMessage($"Неожиданная версия tglcore.dll: {ver} (нужна 3). Обновите native/tglcore.dll."); return; }
        }
        catch (Exception e)
        {
            ShowMessage("Не удалось загрузить native/tglcore.dll:\n" + e.Message);
            return;
        }

        SetupAudio();
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
        ShowMessage(ApplyMods());
        _msgTimer = 5.0;
        tgl_reset();
        _running = true;
        return true;
    }

    double _msgTimer;

    // Моды: каждая строка .tglpatch — «банк адрес байты…» (hex), «translation off» или комментарий «#».
    // Правки ложатся на копию ROM в ядре; файл ROM на диске не меняется.
    string ApplyMods()
    {
        string dir = ProjectSettings.GlobalizePath("res://mods");
        if (!System.IO.Directory.Exists(dir)) return "";
        var files = System.IO.Directory.GetFiles(dir, "*.tglpatch");
        Array.Sort(files);
        if (files.Length == 0) return "";
        var report = new System.Text.StringBuilder("Моды:\n");
        bool translation = true;
        foreach (var f in files)
        {
            int ok = 0, bad = 0;
            foreach (var raw in System.IO.File.ReadAllLines(f))
            {
                string line = raw.Trim();
                if (line.Length == 0 || line.StartsWith("#")) continue;
                if (line == "translation off") { translation = false; continue; }
                var w = line.Split(' ', StringSplitOptions.RemoveEmptyEntries);
                try
                {
                    int bank = int.Parse(w[0]);
                    int addr = Convert.ToInt32(w[1], 16);
                    var data = new byte[w.Length - 2];
                    for (int i = 0; i < data.Length; i++) data[i] = Convert.ToByte(w[i + 2], 16);
                    if (tgl_patch_rom(bank, addr, data, data.Length) == 0) ok++; else bad++;
                }
                catch (Exception) { bad++; }
            }
            report.Append($"  {System.IO.Path.GetFileNameWithoutExtension(f)}: правок {ok}" + (bad > 0 ? $", ошибок {bad}" : "") + "\n");
        }
        tgl_use_translation(translation ? 1 : 0);
        if (!translation) report.Append("  (мод меняет код — игра идёт на эмуляторе)\n");
        return report.ToString();
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
        if (_msgTimer > 0) { _msgTimer -= delta; if (_msgTimer <= 0) ShowMessage(""); }
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

        PumpAudio();

        if (frames > 0)
        {
            tgl_get_frame_rgba(_rgba);
            _image.SetData(W, H, false, Image.Format.Rgba8, _rgba);
            _texture.Update(_image);
        }
    }

    public override void _UnhandledInput(InputEvent e)
    {
        if (e is InputEventKey k && k.Pressed && !k.Echo)
        {
            if (k.Keycode == Key.F5 && _running) tgl_reset();
            if (k.Keycode == Key.M) _muted = !_muted;
            if (k.Keycode == Key.F7 && _running)
            {
                _running = false;                       // ROM заново (без старых правок) + моды
                TryLoadRom(LoadSetting("rom_path", DefaultRomPath));
            }
        }
    }

    void SetupAudio()
    {
        _mixRate = AudioServer.GetMixRate();
        var gen = new AudioStreamGenerator { MixRate = _mixRate, BufferLength = 0.15f };
        _player = new AudioStreamPlayer { Stream = gen };
        AddChild(_player);
        _player.Play();
        _playback = (AudioStreamGeneratorPlayback)_player.GetStreamPlayback();
        _capacity = _playback.GetFramesAvailable();
        tgl_audio_set_rate(_mixRate);
    }

    // Переложить звук из ядра в Godot. Часы звуковой карты и наши 60,1 кадра/с чуть расходятся,
    // поэтому частота, с которой ядро выдаёт звук, подстраивается (до ±0,5 %) так, чтобы
    // в буфере Godot всегда было ~60 мс: без щелчков от опустошения и без растущей задержки.
    void PumpAudio()
    {
        if (_playback == null) return;
        int n;
        while ((n = tgl_audio_read(_abuf, _abuf.Length)) > 0)
        {
            int room = _playback.GetFramesAvailable();
            if (n > room) n = room;                       // буфер полон — лишнее выбрасываем
            if (n <= 0) continue;
            if (_aframes.Length != n) _aframes = new Vector2[n];
            for (int i = 0; i < n; i++)
            {
                float v = _muted ? 0f : _abuf[i] / 32768f;
                _aframes[i] = new Vector2(v, v);
            }
            _playback.PushBuffer(_aframes);
        }
        int buffered = _capacity - _playback.GetFramesAvailable();
        double target = 0.06 * _mixRate;
        double err = Math.Clamp((target - buffered) / target, -1.0, 1.0);
        tgl_audio_set_rate(_mixRate * (1.0 + 0.005 * err));
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
