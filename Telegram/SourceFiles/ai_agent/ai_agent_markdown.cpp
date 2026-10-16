/*
This file is part of Agentgram, a fork of Telegram Desktop.
For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "ai_agent/ai_agent_markdown.h"

#include <QtCore/QRegularExpression>

namespace AiAgent {
namespace {

struct Open {
	EntityType type = EntityType::Invalid;
	int start = 0;
	QString marker;
};

[[nodiscard]] EntityType MarkerType(const QString &marker) {
	if (marker == u"**"_q || marker == u"__"_q) {
		return EntityType::Bold;
	} else if (marker == u"~~"_q) {
		return EntityType::StrikeOut;
	}
	return EntityType::Italic;
}

void ParseInline(const QString &line, TextWithEntities &out) {
	const auto n = int(line.size());
	auto stack = std::vector<Open>();
	auto i = 0;
	while (i < n) {
		const auto c = line[i];
		if (c == '\\' && i + 1 < n && u"\\`*_~[]()#>"_q.contains(line[i + 1])) {
			out.text += line[i + 1];
			i += 2;
			continue;
		}
		if (c == '`') {
			const auto end = line.indexOf('`', i + 1);
			if (end > i + 1) {
				const auto start = int(out.text.size());
				const auto code = line.mid(i + 1, end - i - 1);
				out.text += code;
				out.entities.push_back(
					{ EntityType::Code, start, int(code.size()) });
				i = end + 1;
				continue;
			}
		}
		if (c == '[') {
			const auto close = line.indexOf(']', i + 1);
			if (close > i && close + 1 < n && line[close + 1] == '(') {
				const auto end = line.indexOf(')', close + 2);
				if (end > close) {
					const auto label = line.mid(i + 1, close - i - 1);
					const auto url = line.mid(
						close + 2,
						end - close - 2).trimmed();
					const auto start = int(out.text.size());
					out.text += label;
					if (!label.isEmpty() && !url.isEmpty()) {
						out.entities.push_back({
							EntityType::CustomUrl,
							start,
							int(label.size()),
							url,
						});
					}
					i = end + 1;
					continue;
				}
			}
		}
		auto marker = QString();
		const auto two = line.mid(i, 2);
		if (two == u"**"_q || two == u"__"_q || two == u"~~"_q) {
			marker = two;
		} else if (c == '*' || c == '_') {
			marker = QString(c);
		}
		if (!marker.isEmpty()) {
			const auto found = ranges::find(
				stack.rbegin(),
				stack.rend(),
				marker,
				&Open::marker);
			const auto prevSpace = (i == 0) || line[i - 1].isSpace();
			if (found != stack.rend() && !prevSpace) {
				const auto open = *found;
				stack.erase(std::next(found).base(), stack.end());
				const auto length = int(out.text.size()) - open.start;
				if (length > 0) {
					out.entities.push_back({ open.type, open.start, length });
				}
				i += marker.size();
				continue;
			}
			const auto after = i + int(marker.size());
			const auto wordBefore = (i > 0) && line[i - 1].isLetterOrNumber();
			const auto canOpen = (after < n)
				&& !line[after].isSpace()
				&& (line.indexOf(marker, after + 1) > after)
				&& !(marker.size() == 1 && wordBefore);
			if (canOpen) {
				stack.push_back({ MarkerType(marker), int(out.text.size()), marker });
				i = after;
				continue;
			}
		}
		out.text += c;
		++i;
	}
}

void AddLinks(TextWithEntities &result) {
	static const auto regex = QRegularExpression(
		u"(https?://[^\\s<>()\\[\\]\"']+[^\\s<>()\\[\\]\"'.,;:!?])"_q);
	auto i = regex.globalMatch(result.text);
	auto added = EntitiesInText();
	while (i.hasNext()) {
		const auto match = i.next();
		const auto start = int(match.capturedStart());
		const auto length = int(match.capturedLength());
		const auto overlaps = ranges::any_of(result.entities, [&](
				const EntityInText &entity) {
			return (entity.offset() < start + length)
				&& (start < entity.offset() + entity.length());
		});
		if (!overlaps) {
			added.push_back({ EntityType::Url, start, length });
		}
	}
	result.entities.append(added);
}

} // namespace

TextWithEntities MarkdownToEntities(const QString &markdown) {
	auto result = TextWithEntities();
	const auto lines = QString(markdown).replace(u"\r\n"_q, u"\n"_q).split('\n');
	auto inCode = false;
	auto codeLanguage = QString();
	auto codeStart = 0;
	auto quoteStart = -1;
	const auto newline = [&] {
		if (!result.text.isEmpty()) {
			result.text += '\n';
		}
	};
	const auto closeQuote = [&] {
		if (quoteStart >= 0) {
			const auto length = int(result.text.size()) - quoteStart;
			if (length > 0) {
				result.entities.push_back(
					{ EntityType::Blockquote, quoteStart, length });
			}
			quoteStart = -1;
		}
	};
	static const auto numbered = QRegularExpression(u"^(\\s*)(\\d+)[.)]\\s+(.*)$"_q);
	static const auto bullet = QRegularExpression(u"^(\\s*)[-*+]\\s+(.*)$"_q);
	static const auto heading = QRegularExpression(u"^#{1,6}\\s+(.*)$"_q);
	static const auto rule = QRegularExpression(u"^\\s*([-*_])(\\s*\\1){2,}\\s*$"_q);
	for (const auto &line : lines) {
		const auto trimmed = line.trimmed();
		if (trimmed.startsWith(u"```"_q)) {
			if (!inCode) {
				closeQuote();
				newline();
				inCode = true;
				codeLanguage = trimmed.mid(3).trimmed();
				codeStart = int(result.text.size());
			} else {
				inCode = false;
				auto length = int(result.text.size()) - codeStart;
				if (length > 0 && result.text.endsWith('\n')) {
					result.text.chop(1);
					--length;
				}
				if (length > 0) {
					result.entities.push_back({
						EntityType::Pre,
						codeStart,
						length,
						codeLanguage,
					});
				}
			}
			continue;
		}
		if (inCode) {
			result.text += line + '\n';
			continue;
		}
		if (trimmed.startsWith('>')) {
			if (quoteStart < 0) {
				newline();
				quoteStart = int(result.text.size());
			} else {
				result.text += '\n';
			}
			ParseInline(trimmed.mid(1).trimmed(), result);
			continue;
		}
		closeQuote();
		newline();
		if (const auto match = heading.match(line); match.hasMatch()) {
			const auto start = int(result.text.size());
			ParseInline(match.captured(1).trimmed(), result);
			const auto length = int(result.text.size()) - start;
			if (length > 0) {
				result.entities.push_back({ EntityType::Bold, start, length });
			}
		} else if (rule.match(line).hasMatch()) {
			result.text += QString(12, QChar(0x2015));
		} else if (const auto match = bullet.match(line); match.hasMatch()) {
			const auto indent = match.captured(1).size() / 2;
			result.text += QString(indent * 2, ' ') + QChar(0x2022) + ' ';
			ParseInline(match.captured(2), result);
		} else if (const auto match = numbered.match(line); match.hasMatch()) {
			const auto indent = match.captured(1).size() / 2;
			result.text += QString(indent * 2, ' ')
				+ match.captured(2)
				+ u". "_q;
			ParseInline(match.captured(3), result);
		} else {
			ParseInline(line, result);
		}
	}
	if (inCode) {
		const auto length = int(result.text.size()) - codeStart;
		if (length > 0) {
			result.entities.push_back({
				EntityType::Pre,
				codeStart,
				length,
				codeLanguage,
			});
		}
	}
	closeQuote();
	while (result.text.endsWith('\n')) {
		result.text.chop(1);
	}
	for (auto &entity : result.entities) {
		const auto end = std::min(
			entity.offset() + entity.length(),
			int(result.text.size()));
		entity.shrinkFromRight(entity.offset() + entity.length() - end);
	}
	result.entities.erase(
		ranges::remove_if(result.entities, [](const EntityInText &entity) {
			return entity.length() <= 0;
		}),
		result.entities.end());
	AddLinks(result);
	ranges::stable_sort(result.entities, std::less<>(), &EntityInText::offset);
	return result;
}

} // namespace AiAgent
