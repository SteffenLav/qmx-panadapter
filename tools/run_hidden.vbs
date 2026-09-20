' run_hidden.vbs - launch a command line with NO window at all.
'
' WHY THIS EXISTS
'   bench.ps1 starts the standing serial capture as a scheduled task, because a
'   capture started any other way from inside a Claude Code tool call gets killed
'   within minutes (CLAUDE.md serial rule 11). But a scheduled task that runs as
'   the logged-on user runs INTERACTIVELY, and an interactive console program
'   always gets a console window. Two things were tried first and both failed:
'
'     powershell -WindowStyle Hidden   only MINIMISES it. The window belongs to
'                                      conhost.exe, not to powershell.exe - which
'                                      is also why checking powershell's
'                                      MainWindowHandle reported 0 and looked
'                                      like success. It sat minimised on the
'                                      operator's taskbar instead.
'     schtasks /ru <user> /np          "run whether logged on or not" would run
'                                      in session 0 with no desktop at all, but
'                                      registering an S4U task needs elevation:
'                                      "Access is denied" from a normal shell.
'
'   WScript.Shell.Run with intWindowStyle 0 does not create a window in the first
'   place, needs no elevation, and is the one approach left.
'
' USAGE
'   wscript.exe run_hidden.vbs <program> [args...]
'   Arguments containing spaces are re-quoted; everything else is passed through.

Option Explicit

Dim sh, cmd, i, a
Set sh = CreateObject("WScript.Shell")

If WScript.Arguments.Count = 0 Then
    WScript.Echo "run_hidden.vbs: nothing to run"
    WScript.Quit 1
End If

cmd = ""
For i = 0 To WScript.Arguments.Count - 1
    a = WScript.Arguments(i)
    ' WScript strips the quotes it was given, so anything with a space has to be
    ' re-quoted or the target program sees it as two separate arguments.
    If InStr(a, " ") > 0 Then a = """" & a & """"
    If i > 0 Then cmd = cmd & " "
    cmd = cmd & a
Next

' 0 = hidden, False = do not wait. Not waiting matters: the capture runs for
' years, and a waiting launcher would keep wscript.exe pinned to it for no gain.
sh.Run cmd, 0, False
WScript.Quit 0
