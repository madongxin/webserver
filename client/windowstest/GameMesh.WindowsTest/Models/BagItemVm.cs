using System.ComponentModel;
using System.Runtime.CompilerServices;

namespace GameMesh.WindowsTest.Models;

public sealed class BagItemVm : INotifyPropertyChanged
{
    private uint _count;

    public ulong ItemId { get; }

    public uint Count
    {
        get => _count;
        set
        {
            if (_count == value) return;
            _count = value;
            OnPropertyChanged();
            OnPropertyChanged(nameof(Display));
        }
    }

    public string Display => $"item_id={ItemId}  count={Count}";

    public BagItemVm(ulong itemId, uint count)
    {
        ItemId = itemId;
        _count = count;
    }

    public event PropertyChangedEventHandler? PropertyChanged;

    private void OnPropertyChanged([CallerMemberName] string? name = null)
        => PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(name));
}
