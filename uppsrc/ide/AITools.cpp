#include "ide.h"

#define LDUMP(x)
#define LLOG(x)

Ide::AIThread Ide::AIChatGPT(const String& instructions, const String& code, const String& prev_id)
{ // Obsolete, work on removal
	Ide::AIThread r;
#ifdef _DEBUG
//	HttpRequest::Trace();
#endif

	HttpRequest http("https://api.openai.com/v1/responses");

	http.Header("Authorization", "Bearer " + ai_key);

	Upp::Json request;
	request("model", ai_model)
	       ("prompt_cache_key", "experimental_query")
	       ("instructions", instructions)
	       ("input", code)
	       ("max_output_tokens", 100000)
	       ("background", true);

	if(prev_id.GetCount())
		request("previous_response_id", prev_id);

	String responseText = http.ContentType("application/json").Post(request).Execute();
		
	http.WhenWait = [&] {
		if(ai_stop)
			http.Abort();
	};

	if(http.IsError()) {
		r.error << "HTTP request failed: " << http.GetErrorDesc();
		return r;
	}

	Value responseJson = ParseJSON(responseText);

	String responseId = AsString(responseJson["id"]);
	String status = AsString(responseJson["status"]);

	int ms = msecs();
	while(findarg(status, "queued", "in_progress") >= 0) {
		if(ai_stop)
			return r;
		
		Sleep(0);
	
		int ms1 = msecs();
		if(ms1 - ms > 1000) {
			ms = ms1;
			HttpRequest poll("https://api.openai.com/v1/responses/" + responseId);
			poll.Header("Authorization", "Bearer " + ai_key);
			poll.WhenWait = [&] {
				if(ai_stop)
					poll.Abort();
			};
		
			responseText = poll.Execute(); // GET is the default
			if(poll.IsError()) {
				r.error << "Polling HTTP request failed: " << poll.GetErrorDesc();
				return r;
			}
		
			responseJson = ParseJSON(responseText);
			if(IsNull(responseJson)) {
				r.error << "Invalid JSON response while polling: " << responseText;
				return r;
			}
			
			status = AsString(responseJson["status"]);
		}
	}

	LOG("Response status: " << status);

	if(status != "completed") {
		r.error << "Response did not complete. Status: " << status;
		return r;
	}

	if(!IsNull(responseJson["error"])) {
		r.error << "OpenAI API error: " << ~responseJson["error"]["message"];
		return r;
	}

	for(Value item : responseJson["output"])
		if(AsString(item["type"]) == "message") {
			Value content = item["content"];
			for(Value part : content) {
				if(part["type"] == "output_text")
					r.result << part["text"];
			}
		}

	r.response_id = AsString(responseJson["id"]);
/*
	if(AsString(responseJson["status"]) == "incomplete") {
		LOG("Warning: generation was incomplete. Reason: "
		    << AsString(responseJson["incomplete_details"]["reason"]));
	}
*/
	Value usage = responseJson["usage"];
	if(!IsNull(usage)) {
		Value inputDetails = usage["input_tokens_details"];
		PutVerbose(String() << "Input bytes: " << code.GetCount());
		PutVerbose(String() << "Input tokens: " << usage["input_tokens"]);
		PutVerbose(String() << "Cached input tokens: " << inputDetails["cached_tokens"]);
		PutVerbose(String() << "Output tokens: " << usage["output_tokens"]);
		PutVerbose(String() << "Total tokens: " << usage["total_tokens"]);
	}
	
	return r;
}

Ide::AIThread Ide::AIGemini(const String& instructions, const String& code, const String& prev_id)
{
	Ide::AIThread r;
	return r;
#if 0
	HttpRequest http("https://generativelanguage.googleapis.com/v1beta/models/" + ai_model + ":generateContent?key=" + ai_key);

	String responseText =
	http.ContentType("application/json").Post(
		Upp::Json("contents", JsonArray() <<
					Upp::Json("role", "user")
		                     ("parts", JsonArray() << Upp::Json("text", code)
		                                           << Upp::Json("text", instructions)))
	).Execute();

	if (http.IsError()) {
		LLOG("HTTP Request failed: " << http.GetErrorDesc());
		return String::GetVoid();
	}

	Value responseJson = ParseJSON(responseText);
	
	if (!IsNull(responseJson["error"])) {
		LLOG("Gemini API Error: " << responseJson["error"]["message"]);
		return String::GetVoid();
	}

	return ~responseJson["candidates"][0]["content"]["parts"][0]["text"];
#endif
}

Ide::AIThread Ide::AI(const String& instructions, const String& code_, const String& prev_id_)
{
	String code = code_;
	String prev_id = prev_id_;
tool_restart:
	LDUMP(code);
	Ide::AIThread r = ai_kind == 0 ? AIChatGPT(instructions, code, prev_id)
	                               : AIGemini(instructions, code, prev_id);

	auto TimeStamp = [&](FileIn& in) {
		
	};
	LDUMP(r.result);
	int q = r.result.FindAfter("⚡GET ");
	bool stamp = false;
	if(q < 0) {
		stamp = true;
		q = r.result.FindAfter("⚡GET_TIME ");
	}
	if(q >= 0) {
		String path = r.result.Mid(q);
		for(const char *delim : { "\n", "⚡", " " }) {
			int q = path.Find(delim);
			if(q >= 0)
				path.Trim(q);
		}
		LLOG("GET FILE " << path);
		for(String n : GetUppDirs()) {
			FileIn in(n + "/" + path);
			if(in) {
				code << "TIME: " << Time(in.GetTime()) << "\n";
				if(!stamp) {
					String raw;
					int ii = 0;
					while(!in.IsEof()) {
						String l = in.GetLine();
						code << ++ii << ": " << l << "\n";
						raw << l << "\n";
					}
					LDUMP(path);
					ai_file.GetAdd(path) = raw;
					prev_id = r.response_id;
				}
				goto tool_restart;
			}
		}
	}
	return r;
}


struct SummaryAIDlg : WithSummaryAILayout<TopWindow> {
	SummaryAIDlg();
};

SummaryAIDlg::SummaryAIDlg()
{
	CtrlLayoutOKCancel(*this, "AI summary generator");
	
	list.AddColumn("").With([](One<Ctrl>& ctrl) { // 0
		ctrl.Create<Option>().NoWantFocus();
	});
	list.AddColumn("Package"); // 1
	auto fmt = [](const Value& v)->Value {
		Time t = v;
		return IsNull(t) ? Null :
			   Format("%02.2d-%02.2d-%02.2d %02.2d`:%02.2d`:%02.2d",
	                  t.year, t.month, t.day, t.hour, t.minute, t.second);
	};
	list.AddColumn("Package date").ConvertBy(fmt); // 2
	list.AddColumn("Package size"); // 3
	list.AddColumn("Summary date").ConvertBy(fmt); // 4
	list.AddColumn("Summary size"); // 5
	list.AddIndex(); // 6
	list.ColumnWidths("29 180 117 83 117 83");

	const Workspace& wspc = GetIdeWorkspace();
	for(int i = 0; i < wspc.GetCount(); i++) {
		int ii = wspc.use_order[i];
		const Package& pk = wspc.GetPackage(ii);
		String n = wspc[ii];
		Time ltm = Null;
		int64 len = 0;
		for(int i = 0; i < pk.GetCount(); i++) {
			String path = SourcePath(n, pk.file[i]);
			if(GetFileExt(path) != ".ai" && FileExists(path)) {
				FindFile ff(path);
				if(ff) {
					ltm = max(Nvl(ltm, Time::Low()), Time(ff.GetLastWriteTime()));
					len += ff.GetLength();
				}
			}
		}
		String spath = SourcePath(n, "summary.ai");
		int64 l = GetFileLength(spath);
		list.Add(false, n, ltm, AttrText(FormatFileSize(len)).Right(), FileGetTime(spath),
		         AttrText(l >= 0 ? FormatFileSize(l) : String()).Right(),
		         ii);
	}
	
	all << [&] {
		bool set = false;
		for(int i = 0; i < list.GetCount(); i++)
			if(!(bool)list.Get(i, 0))
				set = true;
		for(int i = 0; i < list.GetCount(); i++)
			list.Set(i, 0, set);
	};
	
	date << [this] {
		for(int i = 0; i < list.GetCount(); i++)
			if((Time)list.Get(i, 2) > (Time)list.Get(i, 4))
				list.Set(i, 0, true);
	};
}

String Ide::GetAISummaries()
{
	String summaries;
	const Workspace& wspc = GetIdeWorkspace();
	for(int i = 0; i < wspc.GetCount(); i++)
		summaries << LoadFile(SourcePath(wspc[i], "summary.ai"));
	return summaries;
}

void Ide::AISummary()
{
	SummaryAIDlg dlg;

	if(!dlg.ExecuteOK())
		return;

	const Workspace& wspc = GetIdeWorkspace();
	
	String instructions = R"(
		You have been supplied with a map of C++ source files. You can get the content
		of any file in this map if you reply raw unformatted line
		
		⚡GET file
		
		Provide a short summary of ⚡HEADER - about 200 characters.
		Do not hesitate to check other files (implementation files, @includes,
		check the map for candidates) to improve your summary. If ⚡HEADER is missing,
		do nothing. Do not repeat the header name in the summary."
	)";

	AIThread ai = AI(instructions, GetAIFileMap());
	
	if(ai.error.GetCount()) {
		Exclamation("\1" + ai.error);
		return;
	}

	String base_id = ai.response_id;

	for(int i = 0; i < dlg.list.GetCount(); i++) {
		if(dlg.list.Get(i, 0)) {
			int ii = dlg.list.Get(i, 6);
			String n = wspc[ii];
			const Package& pkg = wspc.GetPackage(ii);
			
			String summary;
			
			for(int i = 0; i < pkg.GetCount(); i++) {
				String hdr = n + "/" + pkg.file[i];
				if(GetFileExt(hdr) == ".h") {
					AIThread ai;
					for(int retry = 0; retry < 3; retry++) {
						ai = AI(instructions, "\n⚡HEADER " + hdr, base_id);
						if(ai.error.GetCount() == 0)
							break;
					}
					if(ai.error.GetCount()) {
						Exclamation("\1" + ai.error);
						return;
					}
					if(ai.result.GetCount())
						summary << hdr << ": " << ai.result << "\n";
				}
			}
			
			Upp::SaveFile(SourcePath(~dlg.list.Get(i, 1), "summary.ai"), summary);
		}
	}
}
