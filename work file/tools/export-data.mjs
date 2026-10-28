/*
 * export-data.mjs —— 把参考站（世界杯网站）的 json/csv 数据导出为 C 服务器可直接读取的文本
 *
 * 用法（在 work file 目录下）：
 *   node tools/export-data.mjs [参考站目录]
 *   参考站目录默认为 D:\编程\世界杯网站
 *
 * 输出（UTF-8，TAB 分隔，每行一条记录，以 # 开头的行为注释，C 端导入时跳过）：
 *   teams.txt   —— 48 支球队
 *   players.txt —— 全部球员
 * 字段缺失时输出空串，由 C 端按 NULL / 0 处理。
 */
import { readFile, writeFile } from "node:fs/promises";
import path from "node:path";
import { fileURLToPath } from "node:url";

const OUT_DIR = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const SRC_DIR = process.argv[2] || "D:\\编程\\世界杯网站";

/* 名单中的位置标题 -> 统一后的位置；日本、瑞典的名单把中场和前锋合写，无法再细分 */
const POSITION_LABELS = [
  ["中场/前锋", "中场/前锋"],
  ["守门员", "门将"],
  ["门将", "门将"],
  ["后卫", "后卫"],
  ["中场", "中场"],
  ["前锋", "前锋"]
];

/* 去掉会破坏 TAB 分隔格式的字符 */
function clean(value) {
  if (value === null || value === undefined) return "";
  return String(value).replace(/[\t\r\n]+/g, " ").trim();
}

function toLine(fields) {
  return fields.map(clean).join("\t");
}

/*
 * 从手工整理的名单（世界杯球员.csv）中推断球员位置
 * 名单格式不统一，但球员总是写在"门将/后卫/中场/前锋"标题之后，
 * 形如"姓名（俱乐部）"。做法：找到"姓名（俱乐部"在全文中的位置，
 * 取它前面最近的一个位置标题；中间若隔着新的球队标题行则视为未知。
 */
function buildPositionFinder(rosterText) {
  const marks = [];
  const labelMap = new Map(POSITION_LABELS);
  const labelRe = new RegExp(POSITION_LABELS.map(([label]) => label).join("|"), "g");

  //正则按列表顺序优先匹配较长的标题（"中场/前锋"、"守门员"）
  for (const m of rosterText.matchAll(labelRe)) {
    marks.push({ at: m.index, pos: labelMap.get(m[0]) });
  }
  //球队标题行，如"韩国："、"巴拿马世界杯名单："、"科特迪瓦队大名单"
  for (const m of rosterText.matchAll(/^[^\s　：:（(，,、]{2,20}(?:名单|名单如下)?[：:]?\s*$/gm)) {
    if (!labelRe.test(m[0]) && /[：:]|名单/.test(m[0])) {
      marks.push({ at: m.index, pos: null });
    }
    labelRe.lastIndex = 0;
  }
  marks.sort((a, b) => a.at - b.at);

  function positionBefore(offset) {
    let found = null;
    for (const mark of marks) {
      if (mark.at >= offset) break;
      found = mark.pos;
    }
    return found;
  }

  const escape = (s) => s.replace(/[.*+?^${}()|[\]\\]/g, "\\$&");

  //先按"姓名（俱乐部"精确定位（重名球员靠俱乐部区分），再退化为"姓名（"；
  //姓名和括号之间允许有空格
  return function findPosition(name, club) {
    const patterns = [
      new RegExp(`${escape(name)}\\s*[（(]\\s*${escape(club)}`),
      new RegExp(`${escape(name)}\\s*[（(]`)
    ];
    for (const re of patterns) {
      const m = re.exec(rosterText);
      if (m) return positionBefore(m.index);
    }
    return null;
  };
}

async function main() {
  const readJson = async (file) => JSON.parse(await readFile(path.join(SRC_DIR, file), "utf8"));

  const [teams, teamsZh, players, rosterText] = await Promise.all([
    readJson("data/teams.json"),
    readJson("data/i18n/teams.zh-CN.json"),
    readJson("data/players.json"),
    readFile(path.join(SRC_DIR, "世界杯球员.csv"), "utf8")
  ]);

  const zhById = new Map(teamsZh.map((t) => [t.teamId, t]));

  //球队：按小组、FIFA 排名排序，方便首页按小组展示
  const teamRows = teams
    .map((t) => ({ ...t, zh: zhById.get(t.id) || {} }))
    .sort((a, b) => (a.group || "").localeCompare(b.group || "")
      || (a.zh.fifaRanking ?? 999) - (b.zh.fifaRanking ?? 999));

  const teamLines = [
    "# team_id\tname_zh\tname_en\tgroup\tcoach\tfifa_rank\tmarket_value(亿)\tplayed\twins\tdraws\tlosses\tpoints\tgoals_for\tgoals_against\tlogo_url",
    ...teamRows.map((t) => toLine([
      t.id, t.zh.nameZh || t.name, t.name, t.group, t.coach,
      t.zh.fifaRanking ?? t.fifaRanking, t.zh.marketValueYi,
      t.matchesPlayed, t.wins, t.draws, t.losses, t.points, t.goalsFor, t.goalsAgainst,
      t.logoUrl
    ]))
  ];

  //球员：位置优先用 json，缺失时从名单中推断
  const findPosition = buildPositionFinder(rosterText);
  const posCount = { 未知: 0 };
  const playerLines = [
    "# name\tteam_id\tposition\tclub\tgoals\tassists",
    ...players.map((p) => {
      const pos = p.position || findPosition(p.name, p.club);
      posCount[pos || "未知"] = (posCount[pos || "未知"] || 0) + 1;
      return toLine([p.name, p.teamId, pos, p.club, p.worldCupGoals ?? 0, p.assists ?? 0]);
    })
  ];

  await writeFile(path.join(OUT_DIR, "teams.txt"), teamLines.join("\n") + "\n", "utf8");
  await writeFile(path.join(OUT_DIR, "players.txt"), playerLines.join("\n") + "\n", "utf8");

  console.log(`teams.txt   : ${teamRows.length} 支球队`);
  console.log(`players.txt : ${players.length} 名球员`);
  console.log("位置分布    :", posCount);
}

main().catch((err) => {
  console.error(err);
  process.exit(1);
});
