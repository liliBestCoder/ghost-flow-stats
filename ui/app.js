"use strict";

const byId = id => document.getElementById(id);
const bytes = new Intl.NumberFormat("zh-CN", { maximumFractionDigits: 2 });
const percent = new Intl.NumberFormat("zh-CN", { style: "percent", maximumFractionDigits: 1 });
const percentWhole = new Intl.NumberFormat("zh-CN", { style: "percent", maximumFractionDigits: 0 });
const units = ["B", "KiB", "MiB", "GiB", "TiB", "PiB"];
const range = byId("range");
const quotaForm = byId("quota-form");
const quotaNode = byId("quota-node");
const quotaLimit = byId("quota-limit");
const quotaSubmit = byId("save-quota");
const gb = 1_000_000_000;
const quotaNumber = new Intl.NumberFormat("zh-CN", { maximumFractionDigits: 6 });
let quotaSignature = "";
let quotaCharts = [];
let savingQuota = false;
let quotaEditorVersion = 0;

function formatBytes(value) {
    let unit = 0;
    while (value >= 1024 && unit < units.length - 1) {
        value /= 1024;
        unit++;
    }
    return `${bytes.format(value)}\u00A0${units[unit]}`;
}

function dates(from, through) {
    const result = [];
    const date = new Date(`${from}T00:00:00Z`);
    while (!Number.isNaN(date.getTime()) && result.length < 180) {
        const day = date.toISOString().slice(0, 10);
        if (day > through) break;
        result.push(day);
        date.setUTCDate(date.getUTCDate() + 1);
    }
    return result;
}

function formatDateRange(from, through) {
    return from === through ? from : `${from} 至 ${through}`;
}

function drawChart(root, rows, direction, resetScroll, hourly = false) {
    const max = Math.max(0, ...rows.map(row => row[direction]));
    const scale = document.createElement("div");
    scale.className = "scale";
    scale.textContent = max ? `最高 ${formatBytes(max)}` : "暂无流量记录";
    const columns = document.createElement("div");
    columns.className = "columns";
    columns.style.gridTemplateColumns = `repeat(${rows.length}, minmax(0, 1fr))`;
    if (rows.length > 30) columns.style.minWidth = `${rows.length * 15}px`;
    const scroller = document.createElement("div");
    scroller.className = "chart-scroll";
    scroller.tabIndex = 0;
    scroller.setAttribute("aria-label", hourly
        ? (direction === "up" ? "每小时上传柱状图" : "每小时下载柱状图")
        : (direction === "up" ? "每日上传柱状图" : "每日下载柱状图"));
    const previous = root.querySelector(".chart-scroll");
    const labelInterval = rows.length > 30 ? 7 : Math.ceil(rows.length / 4);
    for (const [index, row] of rows.entries()) {
        const column = document.createElement("div");
        column.className = "column";
        const label = `${row.label} · ${formatBytes(row[direction])}`;
        const slot = document.createElement("div");
        slot.className = "slot";
        slot.title = label;
        slot.setAttribute("aria-label", label);
        slot.setAttribute("role", "img");
        const bar = document.createElement("span");
        bar.className = "bar";
        bar.style.height = `${max ? row[direction] / max * 100 : 0}%`;
        slot.append(bar);
        const day = document.createElement("time");
        day.dateTime = row.dateTime;
        const showTick = hourly
            ? (index % 6 === 0 || index === rows.length - 1)
            : (index === rows.length - 1 || (index % labelInterval === 0 && rows.length - 1 - index >= labelInterval));
        if (showTick) day.textContent = row.tick;
        column.append(slot, day);
        columns.append(column);
    }
    const position = previous?.scrollLeft ?? 0;
    const atEnd = previous && previous.scrollWidth - previous.clientWidth - position < 2;
    const focused = previous === document.activeElement;
    scroller.append(columns);
    root.replaceChildren(scale, scroller);
    scroller.scrollLeft = resetScroll || atEnd ? scroller.scrollWidth : position;
    if (focused) scroller.focus({ preventScroll: true });
}

function drawRank(root, rows, emptyText) {
    const items = rows.map((row, index) => {
        const item = document.createElement("li");
        const number = document.createElement("span");
        number.className = "position";
        number.textContent = index + 1;
        const detail = document.createElement("div");
        detail.className = "rank-detail";
        const name = document.createElement("span");
        name.textContent = row.name;
        const track = document.createElement("div");
        track.className = "track";
        const bar = document.createElement("span");
        bar.style.width = `${row.bytes / rows[0].bytes * 100}%`;
        track.append(bar);
        detail.append(name, track);
        const value = document.createElement("strong");
        value.textContent = formatBytes(row.bytes);
        item.append(number, detail, value);
        return item;
    });
    if (!items.length) {
        const empty = document.createElement("li");
        empty.className = "empty";
        empty.textContent = emptyText;
        items.push(empty);
    }
    root.replaceChildren(...items);
}

function signedBytes(value) {
    return `${value > 0 ? "+" : value < 0 ? "−" : ""}${formatBytes(Math.abs(value))}`;
}

function drawTotal(direction, value, previous, maximum) {
    byId(`total-${direction}`).textContent = formatBytes(value);
    byId(`total-bar-${direction}`).style.width = `${maximum ? value / maximum * 100 : 0}%`;
    const baseline = previous[direction];
    const delta = value - baseline;
    const changeEl = byId(`change-${direction}`);
    changeEl.textContent = baseline
        ? `${delta > 0 ? "+" : ""}${percent.format(delta / baseline)}`
        : previous.hasRecords ? signedBytes(delta) : "暂无对比";
    changeEl.classList.toggle("is-rise", previous.hasRecords && delta > 0);
    changeEl.classList.toggle("is-drop", previous.hasRecords && delta < 0);
    byId(`previous-${direction}`).textContent = previous.hasRecords
        ? `上期 ${formatBytes(baseline)} · ${signedBytes(delta)}`
        : "上期无流量记录";
}

function changeArrow(rising) {
    const svg = document.createElementNS("http://www.w3.org/2000/svg", "svg");
    svg.setAttribute("viewBox", "0 0 12 12");
    svg.setAttribute("aria-hidden", "true");
    const path = document.createElementNS("http://www.w3.org/2000/svg", "path");
    path.setAttribute("d", rising ? "M2.5 9.5L9.5 2.5M4.5 2.5H9.5V7.5" : "M2.5 2.5L9.5 9.5M4.5 9.5H9.5V4.5");
    svg.append(path);
    return svg;
}

function formatChangeBadge(delta, previous) {
    if (!previous) return "新增";
    const ratio = delta / previous;
    const fmt = Math.abs(ratio) >= 10 ? percentWhole : percent;
    return `${delta > 0 ? "+" : ""}${fmt.format(ratio)}`;
}

function drawChanges(root, rows, hasPrevious) {
    root.replaceChildren();
    if (!hasPrevious || !rows.length) {
        const item = document.createElement("li");
        item.className = "empty";
        item.textContent = hasPrevious ? "用量无变化" : "上一周期无流量记录";
        root.append(item);
        return;
    }
    for (const row of rows) {
        const delta = row.bytes - row.previous;
        const rising = delta > 0;
        const peak = Math.max(row.bytes, row.previous, 1);

        const item = document.createElement("li");
        item.className = rising ? "change-item is-rise" : "change-item is-drop";

        const badge = document.createElement("span");
        badge.className = "change-icon";
        badge.setAttribute("aria-label", rising ? "增加" : "减少");
        badge.append(changeArrow(rising));

        const body = document.createElement("div");
        body.className = "change-body";

        const head = document.createElement("div");
        head.className = "change-head";
        const name = document.createElement("span");
        name.className = "change-name";
        name.textContent = row.name;
        name.title = row.name;
        const deltaText = document.createElement("strong");
        deltaText.className = "change-delta";
        deltaText.textContent = signedBytes(delta);
        head.append(name, deltaText);

        const meterRow = document.createElement("div");
        meterRow.className = "change-meter-row";
        const bars = document.createElement("div");
        bars.className = "change-bars";
        bars.setAttribute("aria-hidden", "true");
        const prevTrack = document.createElement("div");
        prevTrack.className = "change-track";
        const prevBar = document.createElement("span");
        prevBar.className = row.previous > 0 ? "bar-prev non-zero" : "bar-prev";
        prevBar.style.width = `${row.previous / peak * 100}%`;
        prevTrack.append(prevBar);
        const currTrack = document.createElement("div");
        currTrack.className = "change-track";
        const currBar = document.createElement("span");
        currBar.className = row.bytes > 0 ? "bar-curr non-zero" : "bar-curr";
        currBar.style.width = `${row.bytes / peak * 100}%`;
        currTrack.append(currBar);
        bars.append(prevTrack, currTrack);

        const pill = document.createElement("span");
        pill.className = "change-pill";
        pill.textContent = formatChangeBadge(delta, row.previous);
        meterRow.append(bars, pill);

        const meta = document.createElement("div");
        meta.className = "change-meta";
        const prevVal = document.createElement("span");
        prevVal.className = "meta-prev";
        prevVal.textContent = `上期 ${formatBytes(row.previous)}`;
        const arrow = document.createElement("span");
        arrow.className = "meta-arrow";
        arrow.setAttribute("aria-hidden", "true");
        arrow.textContent = "→";
        const currVal = document.createElement("span");
        currVal.className = "meta-curr";
        currVal.textContent = `本期 ${formatBytes(row.bytes)}`;
        meta.append(prevVal, arrow, currVal);

        body.append(head, meterRow, meta);
        item.append(badge, body);
        root.append(item);
    }
}

function drawHeat(rows) {
    const root = byId("heat-map");
    byId("heat-caption").textContent = rows.length
        ? "按星期与小时汇总上传和下载，颜色越深表示用量越高。"
        : "暂无小时级记录，热力图从本版启用后开始采集。";
    root.replaceChildren();
    if (!rows.length) return;
    const labels = ["周日", "周一", "周二", "周三", "周四", "周五", "周六"];
    const sums = new Map(rows.map(row => [row.weekday * 24 + row.hour, row.up + row.down]));
    const peak = Math.max(1, ...sums.values());
    const label = text => {
        const cell = document.createElement("span");
        cell.textContent = text;
        return cell;
    };
    root.append(label(""));
    for (let hour = 0; hour < 24; hour++) root.append(label(hour % 4 === 0 ? String(hour).padStart(2, "0") : ""));
    for (const weekday of [1, 2, 3, 4, 5, 6, 0]) {
        root.append(label(labels[weekday]));
        for (let hour = 0; hour < 24; hour++) {
            const value = sums.get(weekday * 24 + hour);
            const cell = document.createElement("span");
            cell.className = "heat-cell";
            cell.title = `${labels[weekday]} ${hour}:00 · ${value === undefined ? "无记录" : formatBytes(value)}`;
            cell.setAttribute("aria-label", cell.title);
            if (value) cell.style.setProperty("--intensity", `${18 + 82 * Math.sqrt(value / peak)}%`);
            root.append(cell);
        }
    }
}

async function openQuota(quota) {
    if (savingQuota) return;
    const editorVersion = ++quotaEditorVersion;
    quotaForm.hidden = false;
    quotaNode.disabled = true;
    quotaNode.replaceChildren(new Option(quota?.node ?? "正在读取上游节点…", quota?.node ?? ""));
    quotaLimit.value = quota ? quota.limit / gb : "";
    quotaSubmit.disabled = !quota;
    byId("remove-quota").hidden = !quota;
    byId("quota-feedback").textContent = "";
    if (quota) {
        quotaLimit.focus();
        return;
    }
    try {
        const { nodes } = await request({ action: "listQuotaNodes" });
        if (editorVersion !== quotaEditorVersion) return;
        quotaNode.replaceChildren(new Option(nodes.length ? "请选择上游节点" : "没有已配置的上游节点", ""),
                                  ...nodes.map(node => new Option(node.label, node.value)));
        quotaNode.disabled = nodes.length === 0;
        quotaSubmit.disabled = nodes.length === 0;
        if (nodes.length) quotaNode.focus();
    } catch {
        if (editorVersion !== quotaEditorVersion) return;
        quotaNode.replaceChildren(new Option("上游节点读取失败", ""));
        byId("quota-feedback").textContent = "请确认插件已获得配置读取权限，然后重新打开配额设置。";
    }
}

function drawCurve({ chart, caption, limit, days, points }) {
    const width = chart.clientWidth;
    const height = chart.clientHeight;
    if (!width || !height) return;
    const ratio = window.devicePixelRatio || 1;
    chart.width = Math.round(width * ratio);
    chart.height = Math.round(height * ratio);
    const ctx = chart.getContext("2d");
    ctx.scale(ratio, ratio);
    const peak = Math.max(limit, points.at(-1)) * 1.12;
    const colors = getComputedStyle(chart);
    const x = i => 6 + i / days.length * (width - 12);
    const y = value => height - 14 - value / peak * (height - 26);
    ctx.strokeStyle = colors.borderColor;
    ctx.beginPath();
    ctx.moveTo(x(0), y(0));
    ctx.lineTo(x(days.length), y(0));
    ctx.stroke();
    ctx.setLineDash([5, 4]);
    ctx.strokeStyle = getComputedStyle(caption).color;
    ctx.beginPath();
    ctx.moveTo(x(0), y(limit));
    ctx.lineTo(x(days.length), y(limit));
    ctx.stroke();
    ctx.setLineDash([]);
    ctx.strokeStyle = colors.color;
    ctx.lineWidth = 2;
    ctx.beginPath();
    ctx.moveTo(x(0), y(0));
    points.slice(1).forEach((value, i) => {
        ctx.lineTo(x(i + 1), y(points[i]));
        ctx.lineTo(x(i + 1), y(value));
    });
    ctx.stroke();
    chart.onpointermove = event => {
        const i = Math.max(1, Math.min(days.length, Math.round((event.offsetX - 6) / (width - 12) * days.length)));
        caption.textContent = `${days[i - 1]} · 累计 ${formatBytes(points[i])}`;
    };
    const summary = `${days[0].slice(5)} 至 ${days.at(-1).slice(5)} · 虚线为配额`;
    chart.onpointerleave = () => { caption.textContent = summary; };
    caption.textContent = summary;
}

function drawQuotaCurves() {
    quotaCharts.forEach(drawCurve);
}

function drawQuotas(data) {
    const signature = JSON.stringify([data.monthStart, data.today, data.quotas, data.quotaDaily]);
    if (signature === quotaSignature) return;
    quotaSignature = signature;
    byId("quota-month").textContent = `${data.monthStart.slice(0, 7)} · 自然月 · 按本地记录的上传和下载合计`;
    const root = byId("quotas");
    quotaObserver.disconnect();
    quotaCharts = [];
    root.replaceChildren();
    if (!data.quotas.length) {
        const empty = document.createElement("p");
        empty.className = "panel quota-empty section-note";
        empty.textContent = "尚未设置节点配额。";
        root.append(empty);
    }
    const days = dates(data.monthStart, data.today);
    for (const quota of data.quotas) {
        const used = quota.up + quota.down;
        const remaining = quota.limit - used;
        const usagePercent = percent.format(used / quota.limit);
        const card = document.createElement("article");
        card.className = "panel quota-card";
        const header = document.createElement("header");
        header.className = "quota-row-header";
        const title = document.createElement("h3");
        title.textContent = quota.node;
        const edit = document.createElement("button");
        edit.type = "button";
        edit.className = "button";
        edit.textContent = "编辑";
        edit.setAttribute("aria-label", `编辑 ${quota.node} 配额`);
        edit.onclick = () => openQuota(quota);
        const share = document.createElement("span");
        share.className = remaining < 0 ? "quota-percent over-limit" : "quota-percent";
        share.textContent = `已用 ${usagePercent}`;
        header.append(title, share, edit);
        const amounts = document.createElement("div");
        amounts.className = "quota-amounts";
        const usage = document.createElement("strong");
        usage.textContent = `已用 ${formatBytes(used)} / ${quotaNumber.format(quota.limit / gb)} GB`;
        const bar = document.createElement("progress");
        bar.max = quota.limit;
        bar.value = Math.min(used, quota.limit);
        bar.setAttribute("aria-label", `${quota.node} 已用配额 ${usagePercent}`);
        if (remaining < 0) bar.className = "over-limit";
        const detail = document.createElement("span");
        detail.textContent = `${remaining < 0 ? "超出" : "剩余"} ${formatBytes(Math.abs(remaining))}`;
        amounts.append(usage, detail);
        const trend = document.createElement("div");
        trend.className = "quota-trend";
        const trendLabel = document.createElement("span");
        trendLabel.className = "quota-trend-label";
        trendLabel.textContent = "本月累计用量";
        const chart = document.createElement("canvas");
        chart.setAttribute("role", "img");
        chart.setAttribute("aria-label", `${quota.node} 本月每日累计用量和配额线`);
        const caption = document.createElement("p");
        caption.className = "section-note quota-curve-detail";
        trend.append(trendLabel, chart, caption);
        card.append(header, bar, amounts, trend);
        root.append(card);
        const records = Object.hasOwn(data.quotaDaily, quota.node) ? data.quotaDaily[quota.node] : [];
        const known = new Map(records.map(row => [row.day, row.up + row.down]));
        let cumulative = 0;
        const points = [0, ...days.map(day => (cumulative += known.get(day) || 0))];
        quotaCharts.push({ chart, caption, limit: quota.limit, days, points });
        quotaObserver.observe(chart);
    }
    drawQuotaCurves();
}

async function request(body) {
    const response = await fetch("./api", {
        method: "POST",
        headers: { "Content-Type": "application/json", "X-Ghost-Flow-UI": "1" },
        body: JSON.stringify(body)
    });
    const data = await response.json();
    if (!response.ok || data.status !== "ok") throw new Error(data.error || "history unavailable");
    return data;
}

let pending = false;
let lastDashboard = "";
let displayedRange = 0;
let reloadQueued = false;
async function load() {
    if (document.hidden || pending) return;
    pending = true;
    const requestedDays = Number(range.value);
    const collector = byId("collector");
    try {
        const data = await request({ days: requestedDays });
        if (Number(range.value) !== requestedDays) return;
        collector.textContent = data.collecting ? "正在采集" : "等待采样";
        collector.classList.toggle("active", data.collecting);
        collector.title = data.lastSample
            ? `最近采样 ${new Date(data.lastSample * 1000).toLocaleString("zh-CN")}`
            : "尚未取得有效样本";
        const { collecting, lastSample, ...dashboard } = data;
        const signature = JSON.stringify(dashboard);
        if (signature === lastDashboard) return;
        lastDashboard = signature;
        const maximum = Math.max(data.up, data.down);
        drawTotal("up", data.up, data.previous, maximum);
        drawTotal("down", data.down, data.previous, maximum);
        byId("previous-range").textContent = `较上期 ${formatDateRange(data.previous.from, data.previous.through)}`;
        drawChanges(byId("process-changes"), data.processChanges, data.previous.hasRecords);
        drawChanges(byId("node-changes"), data.nodeChanges, data.previous.hasRecords);
        drawQuotas(data);
        drawHeat(data.hourly);
        const hourlyChart = requestedDays === 1 && (data.hourly.length > 0 || (data.up === 0 && data.down === 0));
        let chartRows;
        if (hourlyChart) {
            const byHour = new Map(data.hourly.map(row => [row.hour, row]));
            chartRows = Array.from({ length: 24 }, (_, hour) => {
                const hh = String(hour).padStart(2, "0");
                const row = byHour.get(hour);
                return {
                    dateTime: `${data.today}T${hh}:00`,
                    label: `${data.today} ${hh}:00`,
                    tick: hh,
                    up: row ? row.up : 0,
                    down: row ? row.down : 0
                };
            });
        } else {
            const known = new Map(data.daily.map(row => [row.day, row]));
            chartRows = dates(data.fromDay, data.today).map(day => {
                const row = known.get(day);
                return {
                    dateTime: day,
                    label: day,
                    tick: day.slice(5),
                    up: row ? row.up : 0,
                    down: row ? row.down : 0
                };
            });
        }
        byId("daily-title").textContent = hourlyChart ? "每小时流量" : "每日流量";
        byId("chart-up-title").textContent = hourlyChart ? "每小时上传" : "每日上传";
        byId("chart-down-title").textContent = hourlyChart ? "每小时下载" : "每日下载";
        byId("date-range").textContent = formatDateRange(data.fromDay, data.today);
        drawChart(byId("chart-up"), chartRows, "up", displayedRange !== requestedDays, hourlyChart);
        drawChart(byId("chart-down"), chartRows, "down", displayedRange !== requestedDays, hourlyChart);
        displayedRange = requestedDays;
        drawRank(byId("rank-up"), data.topUp, "暂无进程流量记录");
        drawRank(byId("rank-down"), data.topDown, "暂无进程流量记录");
        drawRank(byId("node-up"), data.topNodeUp, "暂无节点流量记录");
        drawRank(byId("node-down"), data.topNodeDown, "暂无节点流量记录");
    } catch {
        if (Number(range.value) !== requestedDays) return;
        collector.textContent = "历史读取失败";
        collector.title = "";
        collector.classList.remove("active");
    } finally {
        pending = false;
        if (reloadQueued || Number(range.value) !== requestedDays) {
            reloadQueued = false;
            load();
        }
    }
}

const quotaObserver = new ResizeObserver(drawQuotaCurves);
load();
setInterval(load, 5000);
document.addEventListener("visibilitychange", load);
range.addEventListener("change", load);
window.matchMedia("(prefers-color-scheme: dark)").addEventListener("change", drawQuotaCurves);
byId("add-quota").addEventListener("click", () => openQuota(null));
byId("cancel-quota").addEventListener("click", () => {
    if (savingQuota) return;
    quotaEditorVersion++;
    quotaForm.hidden = true;
});

async function saveQuota(limit) {
    if (savingQuota) return;
    savingQuota = true;
    const nodeWasDisabled = quotaNode.disabled;
    for (const control of quotaForm.elements) control.disabled = true;
    try {
        await request({ action: "setQuota", node: quotaNode.value, bytes: limit });
        quotaForm.hidden = true;
        if (pending) reloadQueued = true;
        else load();
    } catch (error) {
        byId("quota-feedback").textContent = error.message === "invalid_quota"
            ? "请选择上游节点并填写有效的配额。" : "保存配额失败，请重试。";
    } finally {
        savingQuota = false;
        for (const control of quotaForm.elements) control.disabled = false;
        quotaNode.disabled = nodeWasDisabled;
    }
}

quotaForm.addEventListener("submit", event => {
    event.preventDefault();
    saveQuota(Math.round(quotaLimit.valueAsNumber * gb));
});
byId("remove-quota").addEventListener("click", () => saveQuota(0));
