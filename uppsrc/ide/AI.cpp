#include "ide.h"

#define LDUMP(x) // DUMP(x)
#define LLOG(x)  // LOG(x)

bool DocEditEnter::Key(dword key, int count)
{
	if(key == K_ENTER) {
		WhenEnter();
		return true;
	}
	return DocEdit::Key(findarg(key, K_CTRL|K_ENTER, K_ALT|K_ENTER, K_SHIFT|K_ENTER) >= 0 ? K_ENTER : key, count);
}

void Ide::AI_Start(const String& instructions, const String& prompt, const String& prev_id)
{
	ai_http.NewRequest();
	ai_http.Url("https://api.openai.com/v1/responses")
	       .Header("Authorization", "Bearer " + ai_key)
	       .Timeout(0);

	Upp::Json request;
	request("model", ai_model)
	       ("prompt_cache_key", "experimental_query")
	       ("instructions", instructions)
	       ("input", prompt)
	       ("max_output_tokens", 100000)
	       ("background", true);

	if(prev_id.GetCount())
		request("previous_response_id", prev_id);
	
	ai_http.ContentType("application/json").Post(request);
	
	ai_error.Clear();
	ai_result.Clear();
	ai_working = true;
	
	AI_Do();
}

bool Ide::AI_Do()
{
	if(ai_error.GetCount() || !ai_working)
		return true;

	if(ai_http.Do())
		return false;

	if(ai_http.IsError()) {
		ai_working = false;
		ai_error << "HTTP request failed: " << ai_http.GetErrorDesc();
		return true;
	}

	ai_responseText = ai_http.GetContent();
	ai_responseJson = ParseJSON(ai_responseText);
	ai_responseId = AsString(ai_responseJson["id"]);
	ai_working = false;
	String status = ~ai_responseJson["status"];

	if(findarg(status, "queued", "in_progress") >= 0) {
		ai_http.NewRequest();
		ai_http.Url("https://api.openai.com/v1/responses/" + ai_responseId)
		       .Header("Authorization", "Bearer " + ai_key)
		       .Timeout(0);
		ai_working = true;
		return false;
	}

	if(status != "completed") {
		ai_error << "Response did not complete. Status: " << status;
		return true;
	}

	if(!IsNull(ai_responseJson["error"])) {
		ai_error << "OpenAI API error: " << ~ai_responseJson["error"]["message"];
		return true;
	}

	for(Value item : ai_responseJson["output"])
		if(AsString(item["type"]) == "message") {
			Value content = item["content"];
			for(Value part : content) {
				if(part["type"] == "output_text")
					ai_result << part["text"];
			}
		}

	Value usage = ai_responseJson["usage"];
	if(!IsNull(usage)) {
		Value inputDetails = usage["input_tokens_details"];
		PutVerbose(String() << "Input tokens: " << usage["input_tokens"]);
		PutVerbose(String() << "Cached input tokens: " << inputDetails["cached_tokens"]);
		PutVerbose(String() << "Output tokens: " << usage["output_tokens"]);
		PutVerbose(String() << "Total tokens: " << usage["total_tokens"]);
	}
	
	return true;
}

static String general_instructions = R"(
	You are a C++ code assistant. You receive a map of source files and header summaries. You may also receive a ⚡CURRENT_FILE marker identifying a file and either a cursor line or a line range. The map and marker tell you where to look; they do not establish what the source code says.
	
	1. Identify the task.
	Follow the target named or implied by the user. Otherwise, use ⚡CURRENT_FILE and its indicated line or range. A request such as “explain” means “explain the code at this location.” Use the enclosing declaration, statement, or function as needed. Do not ask for clarification when the target is reasonably clear.
	
	2. Issue retrieval commands yourself.
	The user gives you a task. YOU issue retrieval commands. The host reads your command and returns the result. Never ask the user to type, resend, confirm, or reformat a retrieval command.
	
	If the source lines needed for the task are not present in the conversation, your entire next reply must be:
	⚡GET file
	
	Replace file with the exact path from the map. Emit exactly one raw, unformatted line: no indentation, quotation marks, Markdown, explanation, apology, or second command. End the turn there. Do not say “I’ll check the file”; emit the command.
	
	Example: If the user says “explain,” the cursor is at Core/Util.h:410, and the source lines are absent, your entire reply is:
	⚡GET Core/Util.h
	
	3. Handle cached files correctly.
	The first line of a returned file is its timestamp, not a source line. Use a freshly returned file immediately. If you need to consult a previously retrieved copy again, first emit:
	⚡GET_TIME file. Freshly means since the last user prompt which specified ⚡CURRENT_FILE.
	
	The same one-raw-line rule applies. If the returned timestamp matches the cached timestamp and the needed lines are still present, use the cached source. If the timestamp differs or the needed lines are missing, issue ⚡GET for that file. A timestamp alone is not source content.
	
	4. Continue the original task across retrievals.
	A command is a request, not a result. Do not explain or cite missing code until the host has returned the source. After each result, continue the original task without waiting for another user prompt: either issue the next necessary command or answer from the inspected source.
	
	If you previously promised to check a file but did not emit a command, the retrieval is still pending. On a follow-up such as “really?” or “so?”, emit the pending command instead of apologizing, making another promise, or guessing.
	
	5. Verify behavior against definitions.
	When asked what a function or method does, inspect its definition. A definition in a header is sufficient. If the current source contains only a declaration, use the map to locate likely definition sites, starting with the corresponding implementation file. Retrieve and inspect candidates as needed. Verify the enclosing scope and signature so you do not mistake an overload or similarly named function for the target.
	
	If the definition cannot be found after checking the plausible mapped files, say that you could not find it in the supplied sources. Explain only what the declaration establishes. Do not infer implementation behavior from a name, declaration, or map summary.
	
	6. Answer directly and cite the source.
	Once you have enough evidence, answer the user's request without narrating the retrieval process. Always mark location as ⚡file:line - do not use spaces or `.
	When reporting a bug or issue to fix, mark its location as 🛠file:line - do not use spaces or `. Use the exact mapped path and an inspected source line number. Do not count the timestamp as a source line or invent a citation.
)";

bool Ide::AI_DoMore()
{
	if(AI_Do()) {
		LDUMP(r.result);
		int q = ai_result.FindAfter("⚡GET ");
		bool stamp = false;
		if(q < 0) {
			stamp = true;
			q = ai_result.FindAfter("⚡GET_TIME ");
		}
		if(q >= 0) {
			String path = ai_result.Mid(q);
			for(const char *delim : { "\n", "⚡", " " }) {
				int q = path.Find(delim);
				if(q >= 0)
					path.Trim(q);
			}
			LLOG("GET FILE " << path);
			String code;
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
					}
					
					String s = ~ai_prompt;
					s << "\n" << (stamp ? "GET_TIME " : "GET ") << path;
					ai_prompt <<= s;
					ai_prompt.SetCursor(INT_MAX);
					AI_Start(general_instructions, code, ai_responseId);
					return false;
				}
			}
		}
		
		ai_prev = ai_responseId;
		return true;
	}
	
	return false;
}

String Ide::GetAIFileMap()
{
	String map;
	for(const auto& f : ~CodeIndex()) { // do base classes first
		String n = GetPathNest(f.key);
		if(n.GetCount()) {
			map << "⚡FILE " << UnixPath(f.key.Mid(n.GetCount() + 1)) << "\n";
			String names;
			Index<String> nests;
			for(const AnnotationItem& m : f.value.items) {
				nests.FindAdd(m.nest);
				bool cpp = findarg(GetFileExt(f.key), ".c", ".cpp") >= 0;
				if(m.name.GetCount() && !m.isstatic &&
				   (!cpp && m.kind != CXCursor_MacroDefinition) &&
				   findarg(m.kind, CXCursor_Constructor, CXCursor_Destructor,
				                   CXCursor_ConversionFunction, CXCursor_CXXMethod,
				                   CXCursor_FieldDecl, CXCursor_StructDecl, CXCursor_UnionDecl,
				                   CXCursor_ClassDecl, CXCursor_EnumDecl) < 0 &&
				   *m.name != '(') {
					MergeWith(names, " ", AsString(m.kind) + m.name);
				}
			}
			if(nests.GetCount())
				map << "TYPES " << Join(nests.GetKeys(), " ") << "\n";
			if(names.GetCount())
				map << "NAMES " << names << "\n";
		}
	}
	return map;
}

void Ide::AIPane()
{
	if(ai_pane.GetParent())
		IdeRemoveRight(ai_pane);
	else
		IdeSetRight(ai_pane);
}

void Ide::AIChat()
{
	AIPane();
	ai_prompt.SetFocus();
}

void Ide::AIPrompt()
{
	AIAppendOutput(String() << "&[g@B \1" << ~ai_prompt << "\1]&");
	AIPrompt2(~ai_prompt);
}

void Ide::AIPrompt2(const String& prompt)
{
	SaveFile();
	String p;
	if(IsNull(ai_prev))
		p << GetAIFileMap() << GetAISummaries();
	p << prompt;
	String n = GetPathNest(editfile);
	if(n.GetCount())
		p << "\n⚡CURRENT_FILE " << UnixPath(editfile.Mid(n.GetCount() + 1)) << "\n";
	int l, h;
	if(editor.GetSelection(l, h))
		p << ", lines " << editor.GetLine(l) + 1 << " - " << editor.GetLine(h) + 1 << "\n";
	else
		p << ", line " << editor.GetCursorLine() + 1;

	if(!ai_pane.GetParent())
		AIPane();

	ai_processing_prompt = true;
	AI_Start(general_instructions, p, ai_prev);
	ai_prompt.Disable();
	ai_cancel.Show();
	ai_animation.Show();
}

Image AInimation()
{
	RTIMESTOP("GEN");
	int q = DPI(30);
	ImagePainter p(q, q);
	q /= 2;
	p.Clear();
	double base = msecs() * 0.02;
	double a = DPI(8);
	double b = DPI(4);
	double d = DPI(2) / 240.0;
	for(int i = 0; i < 100; i++) {
		double fi = base + i * 0.1;
		auto CH = [&](double m) { return int((sin(fi * m) + 1) * 127); };
		p.Circle(sin(fi) * a + sin(0.1 * fi) * b + q,
		         cos(0.9 * fi) * a + cos(0.1 * fi) * b + q,
		         i * d)
		  .Fill(100 * Color(CH(1), CH(1.5), CH(7)));
		p.Circle(sin(1.1 * fi) * a + sin(0.3 * fi) * b + q,
		         cos(0.7 * fi) * a  + cos(0.2 * fi) * b + q,
		         i * d)
		  .Fill(100 * Color(CH(2), CH(0.5), CH(3)));
	}
	return p;
}

void Ide::AIPeriodic()
{
	if(ai_processing_prompt) {
		ai_animation.SetImage(AInimation());
		if(AI_DoMore()) {
			if(ai_error.GetCount())
				AIAppendOutput("[@r " + ai_error);
			else
				AIAppendOutput(AIResult());
			ai_processing_prompt = false;
			ai_prompt.Enable();
			ai_prompt <<= Null;
			ai_cancel.Hide();
			ai_animation.Hide();
			ai_prompt.SetFocus();
		}
	}
	else
		ai_prompt.Enable();
}

String Ide::AIResult()
{
	String qtf = MarkdownConverter().ToQtf(ai_result);
	for(;;) {
		String hdr = "⚡";
		int l = qtf.Find(hdr);
		bool fix = false;
		if(l < 0) {
			hdr = "🛠";
			l = qtf.Find(hdr);
			if(l < 0)
				break;
			fix = true;
		}
		int sc = qtf.FindAfter(":", l);
		if(sc < 0)
			break;
		while(IsDigit(qtf[sc]))
			sc++;
		String link = Filter(qtf.Mid(l + hdr.GetCount(), sc - l - hdr.GetCount()),
		                     [](int c) { return c == '`' ? 0 : c; });
		qtf.Remove(l, sc - l);
		String s = "[^" + link + "^ \1" + link + "\1]";
		if(fix)
			s << " [^!" + link + "^ Fix]";
		qtf.Insert(l, s);
	}
	ai_prev = ai_responseId;
	return qtf;
}

void Ide::AIAppendOutput(const String& qtf)
{
	ai_qtf << qtf;
	ai_output.SetQTF(ai_qtf);
	ai_output.ScrollEnd();
}

void Ide::AILink(const String& link)
{
	if(*link == '!') {
		String path = link.Mid(1);
		if(!GotoPosition(path))
			return;
		int q = path.Find(':');
		if(q >= 0)
			path.Trim(q);
		LLOG("FIX " << path);
		String raw = ai_file.Get(path, editor.Get());
		if(IsNull(raw)) {
			AIAppendOutput("[g Uknown file: \1" + path);
			return;
		}
		AIThread r = AI(general_instructions,
		                "Suggest the fix for issue " + path + ". Fix should be a continous range of lines. "
		                "Show the range of lines you are replacing with line1:line2 range in the first line. "
		                "Do not add any text or explanation, just the required header and the raw code."
		                "Do not use any formatting, I will be using the code as is.",
		                ai_prev);
		ai_prev = r.response_id;
		int p = r.result.Find('\n');
		if(p > 0) {
			String h = r.result.Mid(0, p);
			String code = r.result.Mid(p + 1);
			h.TrimStart("⚡");
			CParser p(h);
			int line1, line2;
			try {
				line1 = p.ReadInt() - 1;
				p.Char(':');
				line2 = p.ReadInt() - 1;
				Vector<String> l = Split(raw, '\n', false);
				
				
				
				if(line1 <= line2 && line1 < l.GetCount() && line2 < l.GetCount() && line1 >= 0) {
					l.Remove(line1, line2 - line1 + 1);
					Vector<String> fix = Split(code, '\n', false);
					l.Insert(line1, fix);
					
					editor.SetSelection(editor.GetPos(line1), editor.GetPos(line2));
					static FileSel dummy;
				    FileDiff& dlg = CreateNewWindow<FileDiff>(dummy);
				    String left = editor.Get();
				    String right = Join(l, "\n");
				    dlg.Set(editfile);
				    dlg.r <<= "Proposed fix";
				    dlg.r.Disable();
				    dlg.diff.Set(left, right);
				    dlg.diff.indent <<= false;
				    dlg.diff.indent << [=, &dlg] { dlg.diff.Set(left, right); };
					dlg.Title("Suggested fix");
					dlg.Icon(IdeImg::DiffClip());
				    dlg.OpenMain();
					return;
				}
			}
			catch(CParser::Error) {}
		}
		AIAppendOutput("[g@R Unexpected reply:&][g \1" + r.result + "\1]");
		return;
	}
	GotoPosition(link);
}
