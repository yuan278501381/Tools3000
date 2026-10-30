/* ─────────────────────────────────────────────────────────────────────────────
 * ScopeRulesManager — 作用域规则管理 (按进程 / 类名 启用·禁用·切换配置集)
 *
 * 自包含: 挂载时加载 gesture.getScopeRules, 变更后经 gesture.updateScopeRules 持久化。
 * 支持批量勾选删除。
 * ───────────────────────────────────────────────────────────────────────────── */

import { useState, useEffect, useCallback, type FC } from 'react';
import { Card, Button, Badge, Toggle } from './UIKit';
import { ScopeRuleModal } from './ScopeRuleModal';
import { type ScopeRule, EFFECT_KEYS, MATCH_MODE_KEYS, ruleTarget } from './scopeModel';
import { bridgeRequest } from '../hooks/useBridge';
import { useTranslation } from 'react-i18next';
import { toast } from 'sonner';
import { Edit3, Trash2 } from 'lucide-react';

interface Props {
  profileNames: string[];
}

export const ScopeRulesManager: FC<Props> = ({ profileNames }) => {
  const { t } = useTranslation();
  const [rules, setRules] = useState<ScopeRule[]>([]);
  const [selected, setSelected] = useState<Set<string>>(new Set());
  const [editorOpen, setEditorOpen] = useState(false);
  const [editing, setEditing] = useState<ScopeRule | null>(null);

  useEffect(() => {
    bridgeRequest<ScopeRule[]>('gesture.getScopeRules')
      .then((data) => setRules(Array.isArray(data) ? data : []))
      .catch((err) => {
        console.error('Failed to load scope rules:', err);
        toast.error(t('scope.loadFailed'));
      });
  }, [t]);

  const persist = useCallback(async (next: ScopeRule[]) => {
    const prev = rules;
    setRules(next);
    try {
      const result = await bridgeRequest<{ success: boolean; error?: string }>('gesture.updateScopeRules', { rules: next });
      if (!result.success) throw new Error(result.error || t('scope.saveFailed'));
    } catch (err) {
      console.error('Failed to save scope rules:', err);
      setRules(prev); // 回滚
      toast.error(t('scope.saveFailed'), { description: String(err) });
    }
  }, [rules, t]);

  const openAdd = () => { setEditing(null); setEditorOpen(true); };
  const openEdit = (r: ScopeRule) => { setEditing(r); setEditorOpen(true); };

  const handleSave = (saved: ScopeRule) => {
    const idx = rules.findIndex((r) => r.id === saved.id);
    const next = [...rules];
    if (idx >= 0) next[idx] = saved; else next.push(saved);
    persist(next);
    setEditorOpen(false);
  };

  const toggleEnabled = (r: ScopeRule, enabled: boolean) =>
    persist(rules.map((x) => (x.id === r.id ? { ...x, enabled } : x)));

  const deleteOne = (r: ScopeRule) => {
    if (!window.confirm(t('scope.deleteRuleConfirm', { name: r.name }))) return;
    persist(rules.filter((x) => x.id !== r.id));
    setSelected((s) => { const n = new Set(s); n.delete(r.id); return n; });
  };

  const deleteSelected = () => {
    if (selected.size === 0) return;
    if (!window.confirm(t('scope.deleteSelectedConfirm', { count: selected.size }))) return;
    persist(rules.filter((x) => !selected.has(x.id)));
    setSelected(new Set());
  };

  const toggleSelect = (id: string) =>
    setSelected((s) => {
      const n = new Set(s);
      if (n.has(id)) n.delete(id); else n.add(id);
      return n;
    });

  const toggleSelectAll = () =>
    setSelected((s) => (s.size === rules.length ? new Set() : new Set(rules.map((r) => r.id))));

  return (
    <Card>
      <div className="scope-toolbar">
        <span className="scope-toolbar__count">{t('scope.ruleCount', { count: rules.length })}</span>
        <div className="scope-toolbar__actions">
          {selected.size > 0 && (
            <Button size="sm" variant="danger" onClick={deleteSelected}>
              {t('scope.deleteSelected', { count: selected.size })}
            </Button>
          )}
          <Button size="sm" variant="primary" onClick={openAdd}>{t('scope.addRule')}</Button>
        </div>
      </div>

      {rules.length === 0 ? (
        <div className="scope-empty">
          {t('scope.emptyHint')}
        </div>
      ) : (
        <div className="scope-table">
          <div className="scope-row scope-row--head">
            <span className="scope-col scope-col--check">
              <input
                type="checkbox"
                checked={selected.size === rules.length && rules.length > 0}
                onChange={toggleSelectAll}
                aria-label={t('common.selectAll')}
              />
            </span>
            <span className="scope-col scope-col--enabled">{t('scope.colEnabled')}</span>
            <span className="scope-col scope-col--name">{t('scope.colName')}</span>
            <span className="scope-col scope-col--target">{t('scope.colTarget')}</span>
            <span className="scope-col scope-col--effect">{t('scope.colEffect')}</span>
            <span className="scope-col scope-col--actions" />
          </div>

          {rules.map((r) => {
            const target = ruleTarget(r);
            const targetLabel = target.kind === 'process'
              ? t('scope.targetProcess')
              : target.kind === 'class' ? t('scope.targetClass') : t('scope.targetNone');
            return (
              <div key={r.id} className="scope-row">
                <span className="scope-col scope-col--check">
                  <input
                    type="checkbox"
                    checked={selected.has(r.id)}
                    onChange={() => toggleSelect(r.id)}
                    aria-label={t('scope.selectRow', { name: r.name })}
                  />
                </span>
                <span className="scope-col scope-col--enabled">
                  <Toggle id={`scope-${r.id}`} checked={r.enabled} onChange={(v) => toggleEnabled(r, v)} />
                </span>
                <span className="scope-col scope-col--name">{r.name}</span>
                <span className="scope-col scope-col--target">
                  <Badge text={targetLabel} variant="muted" />
                  <code className="scope-target-value">{target.value || t('scope.noTarget')}</code>
                  <span className="scope-matchmode">{MATCH_MODE_KEYS[r.matchMode] ? t(MATCH_MODE_KEYS[r.matchMode]) : '?'}</span>
                </span>
                <span className="scope-col scope-col--effect">
                  <Badge
                    text={EFFECT_KEYS[r.effect] ? t(EFFECT_KEYS[r.effect]) : '?'}
                    variant={r.effect === 0 ? 'success' : r.effect === 1 ? 'danger' : 'primary'}
                  />
                  {r.effect === 2 && r.profileName && (
                    <code className="scope-target-value">{r.profileName}</code>
                  )}
                </span>
                <span className="scope-col scope-col--actions">
                  <button className="gesture-icon-btn" title={t('common.edit')} onClick={() => openEdit(r)}><Edit3 size={16} /></button>
                  <button
                    className="gesture-icon-btn gesture-icon-btn--danger"
                    title={t('common.delete')}
                    onClick={() => deleteOne(r)}
                  ><Trash2 size={16} /></button>
                </span>
              </div>
            );
          })}
        </div>
      )}

      {editorOpen && (
        <ScopeRuleModal
          key={editing?.id ?? '__new__'}
          initial={editing}
          profileNames={profileNames}
          onSave={handleSave}
          onClose={() => setEditorOpen(false)}
        />
      )}
    </Card>
  );
};
