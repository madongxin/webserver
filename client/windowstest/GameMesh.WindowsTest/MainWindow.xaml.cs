using System.Collections.ObjectModel;
using System.Windows;
using System.Windows.Controls;
using Game;
using GameMesh.WindowsTest.Models;
using GameMesh.WindowsTest.Net;

namespace GameMesh.WindowsTest;

public partial class MainWindow : Window
{
    private readonly GameTcpClient _client = new();
    private readonly ObservableCollection<BagItemVm> _bag = new();
    private readonly ObservableCollection<MailBriefVm> _mails = new();
    private string _token = "";
    private ulong _playerId;

    public MainWindow()
    {
        InitializeComponent();
        TxtDeviceId.Text = "win-" + Environment.MachineName + "-" + Environment.TickCount64;
        ListBag.ItemsSource = _bag;
        ListMails.ItemsSource = _mails;
        Closed += (_, _) => _client.Dispose();
    }

    private void Log(string msg)
    {
        var line = $"[{DateTime.Now:HH:mm:ss}] {msg}\n";
        Dispatcher.Invoke(() =>
        {
            TxtLog.AppendText(line);
            TxtLog.ScrollToEnd();
        });
    }

    private void SetConnStatus(string text)
    {
        Dispatcher.Invoke(() => TxtConnStatus.Text = text);
    }

    private bool EnsureConnected()
    {
        if (_client.IsConnected) return true;
        MessageBox.Show(this, "请先连接 Gateway", "提示", MessageBoxButton.OK, MessageBoxImage.Warning);
        return false;
    }

    private bool TryParsePlayerId(out ulong playerId)
    {
        playerId = 0;
        if (!ulong.TryParse(TxtPlayerId.Text.Trim(), out playerId) || playerId == 0)
        {
            MessageBox.Show(this, "请填写有效 player_id", "提示", MessageBoxButton.OK, MessageBoxImage.Warning);
            return false;
        }
        return true;
    }

    private async void RunBusy(Func<Task> work)
    {
        IsEnabled = false;
        try
        {
            await work();
        }
        catch (Exception ex)
        {
            Log("ERROR: " + ex.Message);
            MessageBox.Show(this, ex.Message, "错误", MessageBoxButton.OK, MessageBoxImage.Error);
        }
        finally
        {
            IsEnabled = true;
        }
    }

    private Task<GameResponse> ExchangeAsync(GameRequest req)
        => Task.Run(() => _client.Exchange(req));

    private void BtnConnect_OnClick(object sender, RoutedEventArgs e)
    {
        RunBusy(async () =>
        {
            if (!int.TryParse(TxtPort.Text.Trim(), out var port) || port <= 0)
                throw new InvalidOperationException("无效端口");
            var host = TxtHost.Text.Trim();
            await Task.Run(() => _client.Connect(host, port));
            SetConnStatus($"已连接 {host}:{port}");
            Log($"connected {host}:{port}");
        });
    }

    private void BtnDisconnect_OnClick(object sender, RoutedEventArgs e)
    {
        _client.Disconnect();
        _token = "";
        TxtToken.Text = "";
        SetConnStatus("未连接");
        Log("disconnected");
    }

    private void BtnRegister_OnClick(object sender, RoutedEventArgs e)
    {
        if (!EnsureConnected()) return;
        RunBusy(async () =>
        {
            var req = _client.NewRequest();
            req.Register = new RegisterReq
            {
                DeviceId = TxtDeviceId.Text.Trim(),
                DisplayName = TxtDisplayName.Text.Trim(),
            };
            var rsp = await ExchangeAsync(req);
            if (!rsp.Ok || rsp.Register == null || !rsp.Register.Ok)
            {
                Log($"register fail: {rsp.Message} / {rsp.Register?.Message}");
                throw new InvalidOperationException(rsp.Register?.Message ?? rsp.Message);
            }
            _playerId = rsp.Register.PlayerId;
            Dispatcher.Invoke(() => TxtPlayerId.Text = _playerId.ToString());
            Log($"register ok player_id={_playerId}");
        });
    }

    private void BtnLogin_OnClick(object sender, RoutedEventArgs e)
    {
        if (!EnsureConnected()) return;
        if (!TryParsePlayerId(out var playerId)) return;
        RunBusy(async () =>
        {
            var req = _client.NewRequest();
            req.Login = new LoginReq
            {
                PlayerId = playerId,
                DeviceId = TxtDeviceId.Text.Trim(),
                ServerId = 1,
                KickOtherDevice = true,
            };
            var rsp = await ExchangeAsync(req);
            if (!rsp.Ok || rsp.Login == null || !rsp.Login.Ok)
            {
                Log($"login fail: {rsp.Message} / {rsp.Login?.Message}");
                throw new InvalidOperationException(rsp.Login?.Message ?? rsp.Message);
            }
            _playerId = playerId;
            _token = rsp.Login.Token;
            Dispatcher.Invoke(() => TxtToken.Text = _token);
            Log($"login ok token_len={_token.Length} session={rsp.Login.SessionId} server={rsp.Login.ServerId}");
        });
    }

    private void BtnLogout_OnClick(object sender, RoutedEventArgs e)
    {
        if (!EnsureConnected()) return;
        if (!TryParsePlayerId(out var playerId)) return;
        RunBusy(async () =>
        {
            var req = _client.NewRequest(_token);
            req.Logout = new LogoutReq
            {
                PlayerId = playerId,
                Token = _token,
            };
            var rsp = await ExchangeAsync(req);
            Log($"logout ok={rsp.Ok} msg={rsp.Message} body={rsp.Logout?.Message}");
            _token = "";
            Dispatcher.Invoke(() => TxtToken.Text = "");
        });
    }

    private void BtnGrant_OnClick(object sender, RoutedEventArgs e)
    {
        if (!EnsureConnected()) return;
        if (!TryParsePlayerId(out var playerId)) return;
        if (string.IsNullOrEmpty(_token))
        {
            MessageBox.Show(this, "请先登录以获取 session_token", "提示", MessageBoxButton.OK, MessageBoxImage.Warning);
            return;
        }
        if (!ulong.TryParse(TxtItemId.Text.Trim(), out var itemId) || itemId == 0)
        {
            MessageBox.Show(this, "无效 item_id", "提示", MessageBoxButton.OK, MessageBoxImage.Warning);
            return;
        }
        if (!uint.TryParse(TxtItemCount.Text.Trim(), out var count) || count == 0)
        {
            MessageBox.Show(this, "无效 count", "提示", MessageBoxButton.OK, MessageBoxImage.Warning);
            return;
        }

        RunBusy(async () =>
        {
            var req = _client.NewRequest(_token);
            req.GrantItem = new GrantItemReq
            {
                PlayerId = playerId,
                ItemId = itemId,
                Count = count,
                ExtraData = "{\"source\":\"windowstest\"}",
            };
            var rsp = await ExchangeAsync(req);
            if (!rsp.Ok || rsp.GrantItem == null || !rsp.GrantItem.Ok)
            {
                Log($"grant fail: {rsp.Message} / {rsp.GrantItem?.Message}");
                throw new InvalidOperationException(rsp.GrantItem?.Message ?? rsp.Message);
            }
            Log($"grant ok item={itemId} bag_total={rsp.GrantItem.BagTotal}");
            Dispatcher.Invoke(() => UpsertBag(itemId, rsp.GrantItem.BagTotal));
        });
    }

    private void UpsertBag(ulong itemId, uint total)
    {
        var existing = _bag.FirstOrDefault(x => x.ItemId == itemId);
        if (existing == null)
            _bag.Add(new BagItemVm(itemId, total));
        else
            existing.Count = total;
    }

    private void AddBagDelta(ulong itemId, uint delta)
    {
        var existing = _bag.FirstOrDefault(x => x.ItemId == itemId);
        if (existing == null)
            _bag.Add(new BagItemVm(itemId, delta));
        else
            existing.Count += delta;
    }

    private async Task RefreshMailListAsync(ulong playerId)
    {
        var req = _client.NewRequest(_token);
        req.MailList = new MailListReq
        {
            PlayerId = playerId,
            Limit = 50,
        };
        var rsp = await ExchangeAsync(req);
        if (!rsp.Ok || rsp.MailList == null || !rsp.MailList.Ok)
        {
            Log($"mail_list fail: {rsp.Message} / {rsp.MailList?.Message}");
            throw new InvalidOperationException(rsp.MailList?.Message ?? rsp.Message);
        }
        Dispatcher.Invoke(() =>
        {
            _mails.Clear();
            foreach (var m in rsp.MailList.Mails)
                _mails.Add(MailBriefVm.From(m));
        });
        Log($"mail_list ok count={rsp.MailList.Mails.Count}");
    }

    private void BtnMailList_OnClick(object sender, RoutedEventArgs e)
    {
        if (!EnsureConnected()) return;
        if (!TryParsePlayerId(out var playerId)) return;
        if (string.IsNullOrEmpty(_token))
        {
            MessageBox.Show(this, "请先登录", "提示", MessageBoxButton.OK, MessageBoxImage.Warning);
            return;
        }

        RunBusy(async () => await RefreshMailListAsync(playerId));
    }

    private void ListMails_OnSelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        // 选中后可点「查看详情」
    }

    private void BtnMailGet_OnClick(object sender, RoutedEventArgs e)
    {
        if (!EnsureConnected()) return;
        if (!TryParsePlayerId(out var playerId)) return;
        if (string.IsNullOrEmpty(_token))
        {
            MessageBox.Show(this, "请先登录", "提示", MessageBoxButton.OK, MessageBoxImage.Warning);
            return;
        }
        if (ListMails.SelectedItem is not MailBriefVm selected)
        {
            MessageBox.Show(this, "请先在列表中选中一封邮件", "提示", MessageBoxButton.OK, MessageBoxImage.Warning);
            return;
        }

        RunBusy(async () =>
        {
            var req = _client.NewRequest(_token);
            req.MailGet = new MailGetReq
            {
                PlayerId = playerId,
                MailId = selected.MailId,
            };
            var rsp = await ExchangeAsync(req);
            if (!rsp.Ok || rsp.MailGet == null || !rsp.MailGet.Ok || rsp.MailGet.Mail == null)
            {
                Log($"mail_get fail: {rsp.Message} / {rsp.MailGet?.Message}");
                throw new InvalidOperationException(rsp.MailGet?.Message ?? rsp.Message);
            }
            var detail = rsp.MailGet.Mail;
            var sb = new System.Text.StringBuilder();
            sb.AppendLine($"mail_id={detail.Brief?.MailId}");
            sb.AppendLine($"title={detail.Brief?.Title}");
            sb.AppendLine($"sender={detail.Brief?.SenderName}");
            sb.AppendLine($"category={detail.Brief?.Category}");
            sb.AppendLine($"read={detail.Brief?.ReadState} attach={detail.Brief?.AttachmentState}");
            sb.AppendLine("--- body ---");
            sb.AppendLine(detail.Body);
            sb.AppendLine("--- attachments ---");
            foreach (var a in detail.Attachments)
            {
                sb.AppendLine(
                    $"  slot={a.SlotIndex} type={a.AssetType} id={a.AssetId} count={a.Count} state={a.ClaimState}");
            }
            if (detail.AllowedActions.Count > 0)
                sb.AppendLine("actions: " + string.Join(",", detail.AllowedActions));

            Dispatcher.Invoke(() => TxtMailDetail.Text = sb.ToString());
            Log($"mail_get ok id={selected.MailId}");

            // 顺带标记已读
            var readReq = _client.NewRequest(_token);
            readReq.MailRead = new MailReadReq
            {
                PlayerId = playerId,
                MailId = selected.MailId,
                IdempotencyKey = Guid.NewGuid().ToString("N"),
            };
            var readRsp = await ExchangeAsync(readReq);
            Log($"mail_read ok={readRsp.Ok} msg={readRsp.MailRead?.Message}");
        });
    }

    private void BtnMailClaim_OnClick(object sender, RoutedEventArgs e)
    {
        if (!EnsureConnected()) return;
        if (!TryParsePlayerId(out var playerId)) return;
        if (string.IsNullOrEmpty(_token))
        {
            MessageBox.Show(this, "请先登录", "提示", MessageBoxButton.OK, MessageBoxImage.Warning);
            return;
        }
        if (ListMails.SelectedItem is not MailBriefVm selected)
        {
            MessageBox.Show(this, "请先在列表中选中一封邮件", "提示", MessageBoxButton.OK, MessageBoxImage.Warning);
            return;
        }

        RunBusy(async () =>
        {
            // 先取详情以便更新本地背包
            var getReq = _client.NewRequest(_token);
            getReq.MailGet = new MailGetReq { PlayerId = playerId, MailId = selected.MailId };
            var getRsp = await ExchangeAsync(getReq);
            var attachments = getRsp.MailGet?.Mail?.Attachments;

            var req = _client.NewRequest(_token);
            req.MailClaim = new MailClaimReq
            {
                PlayerId = playerId,
                MailId = selected.MailId,
                IdempotencyKey = Guid.NewGuid().ToString("N"),
                TraceId = Guid.NewGuid().ToString("N"),
            };
            var rsp = await ExchangeAsync(req);
            if (!rsp.Ok || rsp.MailClaim == null || !rsp.MailClaim.Ok)
            {
                Log($"mail_claim fail: {rsp.Message} / {rsp.MailClaim?.Message}");
                throw new InvalidOperationException(rsp.MailClaim?.Message ?? rsp.Message);
            }
            Log($"mail_claim ok id={selected.MailId} state={rsp.MailClaim.Result?.AttachmentState}");

            if (attachments != null)
            {
                Dispatcher.Invoke(() =>
                {
                    foreach (var a in attachments)
                    {
                        if (string.Equals(a.AssetType, "ITEM", StringComparison.OrdinalIgnoreCase))
                            AddBagDelta(a.AssetId, a.Count);
                    }
                });
            }

            await RefreshMailListAsync(playerId);
        });
    }

    private void BtnMailDeliver_OnClick(object sender, RoutedEventArgs e)
    {
        if (!EnsureConnected()) return;
        if (!TryParsePlayerId(out var playerId)) return;
        if (string.IsNullOrEmpty(_token))
        {
            MessageBox.Show(this, "请先登录", "提示", MessageBoxButton.OK, MessageBoxImage.Warning);
            return;
        }

        RunBusy(async () =>
        {
            var req = _client.NewRequest(_token);
            var deliver = new MailDeliverReq
            {
                SourceSystem = "windowstest",
                BusinessKey = "win-" + Guid.NewGuid().ToString("N"),
                ReceiverType = "ROLE",
                ReceiverId = playerId,
                Category = "system",
                Priority = 0,
                SenderName = "WindowsTest",
                Title = TxtMailTitle.Text.Trim(),
                Body = TxtMailBody.Text,
                TraceId = Guid.NewGuid().ToString("N"),
            };
            if (ChkMailAttach.IsChecked == true)
            {
                if (!ulong.TryParse(TxtMailAttachItem.Text.Trim(), out var assetId) || assetId == 0)
                    throw new InvalidOperationException("无效附件 item_id");
                if (!uint.TryParse(TxtMailAttachCount.Text.Trim(), out var cnt) || cnt == 0)
                    throw new InvalidOperationException("无效附件数量");
                deliver.Attachments.Add(new MailDeliverAttachment
                {
                    AssetType = "ITEM",
                    AssetId = assetId,
                    Count = cnt,
                    BindType = "NONE",
                });
            }
            req.MailDeliver = deliver;
            var rsp = await ExchangeAsync(req);
            if (!rsp.Ok || rsp.MailDeliver == null || !rsp.MailDeliver.Ok)
            {
                Log($"mail_deliver fail: {rsp.Message} / {rsp.MailDeliver?.Message}");
                throw new InvalidOperationException(rsp.MailDeliver?.Message ?? rsp.Message);
            }
            Log($"mail_deliver ok mail_id={rsp.MailDeliver.MailId} idempotent={rsp.MailDeliver.IdempotentHit}");
            await RefreshMailListAsync(playerId);
        });
    }
}
