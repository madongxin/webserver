using Game;

namespace GameMesh.WindowsTest.Models;

public sealed class MailBriefVm
{
    public ulong MailId { get; init; }
    public string Title { get; init; } = "";
    public string Sender { get; init; } = "";
    public string ReadState { get; init; } = "";
    public string AttachmentState { get; init; } = "";
    public bool HasAttachment { get; init; }
    public string Category { get; init; } = "";
    public string Display =>
        $"#{MailId} [{Category}] {Title} | {Sender} | read={ReadState} attach={AttachmentState}";

    public static MailBriefVm From(MailBrief m) => new()
    {
        MailId = m.MailId,
        Title = m.Title,
        Sender = m.SenderName,
        ReadState = m.ReadState,
        AttachmentState = m.AttachmentState,
        HasAttachment = m.HasAttachment,
        Category = m.Category,
    };
}
