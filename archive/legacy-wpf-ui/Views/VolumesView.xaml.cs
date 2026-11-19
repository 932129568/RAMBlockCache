using System.Collections.ObjectModel;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Data;
using RamBoostFS.Wpf.Models;
using RamBoostFS.Wpf.Native;
using RamBoostFS.Wpf.ViewModels;

namespace RamBoostFS.Wpf.Views;

/// <summary>单卷行 VM（VolumesView ItemsControl 的绑定项；状态位来自 RbVolumeInfo）。</summary>
public class VolumeItem : ObservableObject
{
    public char LetterChar { get; set; }
    public string LetterLabel => $"{LetterChar}:";
    public string Title => $"{LetterChar}: 盘";
    public bool IsSystem { get; set; }
    public Visibility IsSystemVis => IsSystem ? Visibility.Visible : Visibility.Collapsed;

    private bool _bound;
    public bool Bound
    {
        get => _bound;
        set { SetField(ref _bound, value); OnPropertyChanged(nameof(BoundLabel)); OnPropertyChanged(nameof(BoundBadgeVis)); }
    }
    public string BoundLabel => _bound ? "已绑定到卷栈" : "未绑定";
    /* [P3] 旧实现恒 Visible：未绑定卷也显示"已绑定"徽标。按 Bound 显隐。 */
    public Visibility BoundBadgeVis => _bound ? Visibility.Visible : Visibility.Collapsed;

    private bool _attached;
    public bool Attached
    {
        get => _attached;
        set { SetField(ref _attached, value); OnPropertyChanged(nameof(AttachedLabel)); OnPropertyChanged(nameof(AttachedBadgeVis)); OnPropertyChanged(nameof(AttachBtnText)); }
    }
    public string AttachedLabel => Attached ? "已加速" : "未加速";
    public Visibility AttachedBadgeVis => Attached ? Visibility.Visible : Visibility.Collapsed;
    public string AttachBtnText => Attached ? "分离" : "附加";

    private bool _readOn;
    public bool ReadOn { get => _readOn; set => SetField(ref _readOn, value); }
    private bool _writeOn;
    public bool WriteOn { get => _writeOn; set => SetField(ref _writeOn, value); }

    private uint _writeMode;
    public uint WriteMode
    {
        get => _writeMode;
        set { if (SetField(ref _writeMode, value)) OnPropertyChanged(nameof(WriteModeIndex)); }
    }
    // ComboBox: 0=直写 1=回写 2=跟随全局
    public int WriteModeIndex
    {
        get => (int)_writeMode;
        set { if (_writeMode != (uint)value) { _writeMode = (uint)value; OnPropertyChanged(); } }
    }

    public string CapacityText { get; set; } = "";
    public string SectorText { get; set; } = "";
}

/// <summary>
/// 卷管理页：GET_VOLUMES 枚举 + 每卷读/写开关、写模式（直写/回写/跟随全局）、
/// 运行时热附加/分离（ATTACH_VOLUME）；容量取不到时 GetDiskFreeSpaceExW 兜底。
/// </summary>
public partial class VolumesView : UserControl
{
    private RbSession? _s;
    public ObservableCollection<VolumeItem> Items { get; } = new();

    public VolumesView()
    {
        InitializeComponent();
        volumeList.ItemsSource = Items;
    }

    public void SetSession(RbSession s)
    {
        _s = s;
        LoadVolumes();
    }

    private async void LoadVolumes()
    {
        /* [FIX B31] 空态显隐：末尾与两个早退分支都切换（旧实现空列表=空白区）。 */
        try
        {
        if (_s == null) { SyncEmpty(); return; }
        var list = await _s.Client.GetVolumesAsync();
        Items.Clear();
        if (list == null) { SyncEmpty(); return; }
        var v = list.Value;
        for (int i = 0; i < v.Count && i < RbProtoDefs.RB_MAX_VOLUMES; i++)
        {
            var info = v.Items[i];
            char letter = (char)info.Letter;
            if (letter < 'A' || letter > 'Z') continue;

            /* FIX[R4]: 内核可能填不出容量/空闲 —— 用户态 GetDiskFreeSpaceEx 兜底补全 */
            ulong capMB = info.CapacityMB, freeMB = info.FreeMB;
            if (NativeMethods.GetDiskFreeSpaceExW($"{letter}:\\",
                    out ulong availB, out ulong totalB, out ulong freeB))
            {
                ulong userCap  = totalB / (1024UL * 1024UL);
                ulong userFree = availB / (1024UL * 1024UL);
                if (capMB == 0) capMB = userCap;
                if (freeMB == 0) freeMB = userFree;
            }
            string cap = capMB == 0 ? "容量未知" :
                $"容量 {capMB / 1024.0:0} GB · 可用 {freeMB / 1024.0:0} GB";

            int logSec = info.LogicalSectorBytes == 0 ? 512 : (int)info.LogicalSectorBytes;
            int physSec = info.PhysicalSectorBytes == 0 ? logSec : (int)info.PhysicalSectorBytes;

            Items.Add(new VolumeItem
            {
                LetterChar = letter,
                IsSystem = info.IsSystem != 0,
                Bound = info.Bound != 0,
                Attached = info.Attached != 0,
                ReadOn = info.ReadEnabled != 0,
                WriteOn = info.WriteEnabled != 0,
                WriteMode = info.WriteMode,
                CapacityText = cap,
                SectorText = $"逻辑扇区 {logSec} B · 物理扇区 {physSec} B",
            });
        }
        SyncEmpty();
        }
        catch (Exception ex)
        {
            /* [FIX B31] 加载异常也刷新空态（旧实现异常时列表停在旧内容、空态不联动） */
            SyncEmpty();
            System.Diagnostics.Debug.WriteLine($"LoadVolumes failed: {ex.Message}");
        }
    }

    /// <summary>[FIX B31] 空态显隐与 Items.Count 联动（失败/空列表均显示引导文案）。</summary>
    private void SyncEmpty()
    {
        if (emptyHint is null) return;
        emptyHint.Visibility = Items.Count == 0 ? Visibility.Visible : Visibility.Collapsed;
    }

    private async void Attach_Click(object sender, RoutedEventArgs e)
    {
        if (_s == null || sender is not Button b || b.Tag is not char letter) return;
        var item = Items.FirstOrDefault(x => x.LetterChar == letter);
        if (item == null) return;
        bool targetAttach = !item.Attached;
        // 启用时应用该卷写模式; 停用时先 flush 脏数据 (驱动侧处理)
        await _s.Client.AttachVolumeAsync(letter, targetAttach, item.ReadOn, item.WriteOn, item.WriteMode);
        await Task.Delay(150);
        LoadVolumes();
    }

    private async void Read_Toggled(object sender, EventArgs e)
    {
        if (_s == null || sender is not Controls.RbToggleSwitch sw || sw.Tag is not char letter) return;
        var item = Items.FirstOrDefault(x => x.LetterChar == letter);
        if (item == null) return;
        await _s.Client.SetVolumeAsync(letter, sw.IsOn, item.WriteOn, item.WriteMode);
        await Task.Delay(150);
        LoadVolumes();
    }

    private async void Write_Toggled(object sender, EventArgs e)
    {
        if (_s == null || sender is not Controls.RbToggleSwitch sw || sw.Tag is not char letter) return;
        var item = Items.FirstOrDefault(x => x.LetterChar == letter);
        if (item == null) return;
        await _s.Client.SetVolumeAsync(letter, item.ReadOn, sw.IsOn, item.WriteMode);
        await Task.Delay(150);
        LoadVolumes();
    }

    private async void WriteMode_Changed(object sender, SelectionChangedEventArgs e)
    {
        if (_s == null || sender is not ComboBox cb || cb.Tag is not char letter) return;
        var item = Items.FirstOrDefault(x => x.LetterChar == letter);
        if (item == null) return;
        uint mode = (uint)cb.SelectedIndex;
        item.WriteMode = mode;
        await _s.Client.SetVolumeAsync(letter, item.ReadOn, item.WriteOn, mode);
    }

    private void Refresh_Click(object sender, RoutedEventArgs e) => LoadVolumes();

    public void LoadVolumesPublic() => LoadVolumes();
}
