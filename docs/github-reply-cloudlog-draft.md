# GitHub reply draft — issue: "Add support for Cloudlog logging API" (g4mem-mark)

Markdown is fine here (unlike the groups.io replies, which are plain text).
NOT posted.

---

Thanks Mark — this is a good fit, and I want to do it.

I've read through the API and it lines up well with what the panadapter already
does for QRZ, eQSL and LoTW: it tracks how far up the log it has uploaded and
sends the rest whenever it next has a network, which is exactly the
operate-offline-then-upload pattern you're describing. Two things in the Cloudlog
API make it easier than the others — it accepts several QSOs in one request, and
it does duplicate checking server-side, so a repeated upload is harmless.

There's one thing I want to settle before I write it, because it decides whether
what I build actually works for you and for everyone else who asks.

**It will be HTTPS only.** Cloudlog and Wavelog are self-hosted, so unlike the
other three logbooks the address has to come from you — and that's the first time
this firmware would send credentials to somewhere it doesn't already know. Your
API key travels in the request body, so on a plain HTTP connection it goes across
in the clear, readable by anything on the path. I'm not willing to ship that as
the normal way to use the feature, or to add a "just don't check the certificate"
switch — the point of a certificate is that it makes that decision for you, and a
switch that turns it off tends to be the setting everybody ends up using.

The practical consequence, and the reason I'm asking rather than assuming:
**HTTPS on its own isn't enough — the certificate has to be one the Tab5 can
verify.** It checks against the standard certificate authorities, the same as it
does for QRZ and LoTW, so a self-signed certificate will be refused even though
the URL says https. That's fine if your instance is behind a real certificate
(Let's Encrypt is free, and a reverse proxy in front of Cloudlog is the usual way
people do it) and it won't work if it isn't.

So: **what does your setup look like?** Is Cloudlog reachable on a hostname with
a proper certificate, or is it plain HTTP on your home network? I'd rather know
before I build it than have you find out afterwards — and if it turns out most
people asking for this are running it on a LAN without a certificate, that's a
different problem worth solving properly rather than papering over.

One thing that may be better than you expect: if your Cloudlog is on your own
network, this becomes the **only** one of the four uploads that needs no internet
at all. Come home from a park, get on your own WiFi, and the QSOs go in — no
outside connection involved. That suits POTA and SOTA better than QRZ, eQSL or
LoTW, which all need the wider world.

And since the address is something you supply, **Wavelog will work too** — the
API path is the same, and the Cloudlog wiki's own example already refers to a
Wavelog URL.

I'll track this as an enhancement. 73 de Stef OZ1LAV
