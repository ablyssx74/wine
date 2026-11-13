// Wine Launcher: opens Windows programs (.exe, .msi, .lnk, .bat) with Wine when
// they are double-clicked in Tracker, and the links of the schemes that Wine knows and Haiku does not
// (steam://, the preferred application of the type application/x-vnd.be.url.steam: Haiku's URL wrapper
// starts it with the link as its argument). Haiku only.
#include <Alert.h>
#include <Application.h>
#include <Entry.h>
#include <OS.h>
#include <Path.h>
#include <Roster.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <string>
#include <vector>

extern char **environ;
static const char *kSignature = "application/x-vnd.Wine-Launcher";

static bool HasSuffix(const std::string &s, const char *suffix)
{
	size_t n = strlen(suffix);
	if (s.size() < n) return false;
	for (size_t i = 0; i < n; i++)
		if (tolower((unsigned char)s[s.size() - n + i]) != suffix[i]) return false;
	return true;
}

// "scheme://something"
static bool IsLink(const char *s)
{
	if (!isalpha((unsigned char)s[0])) return false;
	const char *p = s + 1;
	while (isalnum((unsigned char)*p) || *p == '+' || *p == '-' || *p == '.') p++;
	return p[0] == ':' && p[1] == '/' && p[2] == '/';
}

class LauncherApp : public BApplication {
public:
	LauncherApp() : BApplication(kSignature), fGot(false) {}

	void ReadyToRun() { if (!fGot) PostMessage(B_QUIT_REQUESTED); }
	void RefsReceived(BMessage *msg)
	{
		entry_ref ref; fGot = true;
		for (int32 i = 0; msg->FindRef("refs", i, &ref) == B_OK; i++) {
			BEntry e(&ref, true); BPath p;
			if (e.GetPath(&p) == B_OK) Launch(p.Path());
		}
		PostMessage(B_QUIT_REQUESTED);
	}
	void ArgvReceived(int32 argc, char **argv)
	{
		for (int32 i = 1; i < argc; i++) { fGot = true; Launch(argv[i]); }
		if (fGot) PostMessage(B_QUIT_REQUESTED);
	}

private:
	void Launch(const char *path)
	{
		const char *wine = "/system/apps/wine11/bin/wine";
		if (access(wine, X_OK) != 0) wine = "/boot/system/apps/wine11/bin/wine";
		if (access(wine, X_OK) != 0) {
			(new BAlert("Wine Launcher", "Wine is not installed (wine11 package).", "OK"))->Go();
			return;
		}
		if (IsLink(path)) {
			// the link goes to whatever the Windows side registered for the scheme (Steam's own handler for steam://)
			const char *linkArgs[] = { wine, "start", path, NULL };
			if (chdir("/boot/home") != 0) chdir("/");
			thread_id t = load_image(3, linkArgs, (const char **)environ);
			if (t >= 0) resume_thread(t);
			else (new BAlert("Wine Launcher", "Could not start Wine.", "OK"))->Go();
			return;
		}
		char resolved[B_PATH_NAME_LENGTH];
		std::string file(realpath(path, resolved) ? resolved : path);
		std::string dir = file.substr(0, file.rfind('/'));
		if (dir.empty()) dir = "/";
		std::vector<const char *> args;
		args.push_back(wine);
		// Programs run directly; shortcuts, installers and scripts go through "start".
		if (!HasSuffix(file, ".exe") && !HasSuffix(file, ".com")) {
			args.push_back("start"); args.push_back("/unix");
		}
		args.push_back(file.c_str());
		if (chdir(dir.c_str()) != 0) chdir("/boot/home");
		thread_id t = load_image(args.size(), (const char **)&args[0], (const char **)environ);
		if (t >= 0) resume_thread(t);
		else (new BAlert("Wine Launcher", "Could not start Wine.", "OK"))->Go();
	}
	bool fGot;
};

int main()
{
	LauncherApp app;
	app.Run();
	return 0;
}
