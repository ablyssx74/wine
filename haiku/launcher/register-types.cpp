// Registers the Windows file types and makes Wine Launcher their preferred application.
#include <MimeType.h>
#include <Message.h>
#include <String.h>
#include <stdio.h>
struct T { const char *type, *desc, *ext; };
int main()
{
	const T types[] = {
		{"application/x-dosexec", "Windows program", "exe"},
		{"application/x-ms-installer", "Windows installer", "msi"},
		{"application/x-ms-shortcut", "Windows shortcut", "lnk"},
		{"application/x-ms-batch", "Windows batch file", "bat"},
	};
	for (const T &t : types) {
		BMimeType m(t.type);
		if (!m.IsInstalled()) m.Install();
		BMessage ext(0xea); ext.AddString("extensions", t.ext);
		m.SetFileExtensions(&ext);
		m.SetShortDescription(t.desc);
		m.SetPreferredApp("application/x-vnd.Wine-Launcher");
		printf("%s -> %s\n", t.type, t.ext);
	}
	return 0;
}
