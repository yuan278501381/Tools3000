/**
 * Tools3000 Feedback Relay Worker (Cloudflare Worker 参考实现)
 *
 * 架构特性 (方案 C):
 *   1. 客户端本地已完成 10 维脱敏 + 轻量 SHA-256 PoW (4 个前导零);
 *   2. Worker 端执行 PoW 验签、IP 窗口限流、载荷体积门禁与二次敏感正则清洗;
 *   3. 物理隔离隐私与公开内容:
 *      - 详细脱敏日志/截图存入私有 R2 Bucket (可选绑定 FEEDBACK_R2)，绝不暴露在公网 Issue;
 *      - 仅将清洗后的摘要与环境快照创建为 GitHub Issue，返回追踪单号与直达链接给客户端。
 *
 * 环境变量 (Secrets):
 *   - GITHUB_TOKEN : 仅需 public_repo / issues:write 最小权限 PAT 或 GitHub App Token
 *   - GITHUB_REPO  : 例如 "yuan278501381/tools3000"
 * 可选绑定:
 *   - FEEDBACK_R2  : R2 Bucket (用于私有归档完整脱敏日志与截图)
 */

const MAX_BODY_BYTES = 4 * 1024 * 1024;
const POW_LEADING_ZEROS = 4;
const RATE_LIMIT_MAX_PER_HOUR = 6;
const ipHits = new Map();

async function sha256Hex(text) {
  const bytes = new TextEncoder().encode(text);
  const digest = await crypto.subtle.digest('SHA-256', bytes);
  return [...new Uint8Array(digest)].map((b) => b.toString(16).padStart(2, '0')).join('');
}

function secondPassRedact(text) {
  if (!text || typeof text !== 'string') return '';
  return text
    .replace(/([A-Za-z]:[/\\]Users[/\\])([^/\\\r\n\t"':*?<>|]+)/gi, '$1<USER>')
    .replace(/\b(?:ghp_|gho_|github_pat_|sk-)[A-Za-z0-9_\-]{16,}\b/g, '[REDACTED_KEY]')
    .replace(/(Bearer\s+)[A-Za-z0-9\-._~+/]+=*/gi, '$1[REDACTED_TOKEN]')
    .replace(/\b[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\.[A-Za-z]{2,}\b/g, '[REDACTED_EMAIL]')
    .replace(/\bS-1-5-21-\d+-\d+-\d+-\d+\b/g, 'S-1-5-21-[REDACTED_SID]');
}

function checkRateLimit(ip) {
  const now = Date.now();
  const windowMs = 60 * 60 * 1000;
  const list = (ipHits.get(ip) || []).filter((ts) => now - ts < windowMs);
  if (list.length >= RATE_LIMIT_MAX_PER_HOUR) {
    return false;
  }
  list.push(now);
  ipHits.set(ip, list);
  return true;
}

export default {
  async fetch(request, env) {
    if (request.method !== 'POST') {
      return Response.json({ ok: false, error: 'method_not_allowed' }, { status: 405 });
    }

    const ip = request.headers.get('CF-Connecting-IP') || 'unknown';
    if (!checkRateLimit(ip)) {
      return Response.json({ ok: false, error: 'rate_limited' }, { status: 429 });
    }

    const challenge = request.headers.get('X-Tools3000-Pow-Challenge') || '';
    const nonce = request.headers.get('X-Tools3000-Pow-Nonce') || '';
    if (!challenge.startsWith('t3k-') || !nonce) {
      return Response.json({ ok: false, error: 'missing_pow' }, { status: 400 });
    }

    const challengeTs = Number(challenge.slice(4));
    const nowSec = Math.floor(Date.now() / 1000);
    if (!Number.isFinite(challengeTs) || Math.abs(nowSec - challengeTs) > 900) {
      return Response.json({ ok: false, error: 'expired_pow' }, { status: 400 });
    }

    const powHash = await sha256Hex(`${challenge}:${nonce}`);
    if (!powHash.startsWith('0'.repeat(POW_LEADING_ZEROS))) {
      return Response.json({ ok: false, error: 'invalid_pow' }, { status: 403 });
    }

    const rawBody = await request.text();
    if (rawBody.length > MAX_BODY_BYTES) {
      return Response.json({ ok: false, error: 'payload_too_large' }, { status: 413 });
    }

    let payload;
    try {
      payload = JSON.parse(rawBody);
    } catch {
      return Response.json({ ok: false, error: 'invalid_json' }, { status: 400 });
    }

    const type = String(payload.type || 'bug');
    const title = secondPassRedact(String(payload.title || 'In-App Feedback')).slice(0, 120);
    const description = secondPassRedact(String(payload.description || '')).slice(0, 6000);
    const steps = secondPassRedact(String(payload.stepsToReproduce || '')).slice(0, 3000);
    const envJson = JSON.stringify(payload.environment || {}, null, 2).slice(0, 3000);

    const trackingId = `T3K-${nowSec.toString(36).toUpperCase()}-${powHash.slice(4, 10).toUpperCase()}`;

    // 1. 若绑定了私有 R2 Bucket，将完整脱敏日志与可选截图物理隔离存档（绝不贴到公网 Issue）
    let archivedBundleKey = '';
    if (env.FEEDBACK_R2) {
      archivedBundleKey = `feedback/${new Date().toISOString().slice(0, 10)}/${trackingId}.json`;
      await env.FEEDBACK_R2.put(
        archivedBundleKey,
        JSON.stringify({
          trackingId,
          receivedAt: new Date().toISOString(),
          type,
          title,
          environment: payload.environment || {},
          redactedMainLog: secondPassRedact(String(payload.redactedMainLog || '')),
          redactedServiceLog: secondPassRedact(String(payload.redactedServiceLog || '')),
          hasScreenshot: Boolean(payload.screenshotDataUri),
          screenshotDataUri: typeof payload.screenshotDataUri === 'string' ? payload.screenshotDataUri : '',
        }),
        { httpMetadata: { contentType: 'application/json' } },
      );
    }

    // 2. 创建公开 GitHub Issue（仅含二次脱敏后的描述与环境摘要，附 R2 私有追踪编号）
    const repo = env.GITHUB_REPO || 'yuan278501381/tools3000';
    const labelMap = { bug: 'bug', feature: 'enhancement', performance: 'performance', other: 'feedback' };
    const issueBody = [
      `> Tracking ID: \`${trackingId}\` (Submitted via Tools3000 In-App Feedback)`,
      '',
      '### Description',
      description || '(Empty)',
      steps ? `\n### Steps to Reproduce\n${steps}` : '',
      '\n### Environment Snapshot',
      '```json',
      envJson,
      '```',
      archivedBundleKey ? `\n_Private diagnostic logs archived in R2 key: \`${archivedBundleKey}\`._` : '',
    ]
      .filter(Boolean)
      .join('\n');

    let issueUrl = `https://github.com/${repo}/issues`;
    if (env.GITHUB_TOKEN) {
      const ghResp = await fetch(`https://api.github.com/repos/${repo}/issues`, {
        method: 'POST',
        headers: {
          Authorization: `Bearer ${env.GITHUB_TOKEN}`,
          Accept: 'application/vnd.github+json',
          'User-Agent': 'Tools3000-Feedback-Relay/1.0',
          'Content-Type': 'application/json',
        },
        body: JSON.stringify({
          title: `[In-App ${type}] ${title}`,
          body: issueBody,
          labels: [labelMap[type] || 'feedback'],
        }),
      });
      if (ghResp.ok) {
        const created = await ghResp.json();
        if (created && created.html_url) {
          issueUrl = created.html_url;
        }
      }
    }

    return Response.json({
      ok: true,
      trackingId,
      issueUrl,
    });
  },
};
