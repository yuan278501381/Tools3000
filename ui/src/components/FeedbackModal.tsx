import {
  useState,
  useEffect,
  useCallback,
  useRef,
  type FC,
  type ChangeEvent,
  type ClipboardEvent,
} from 'react';
import { useTranslation } from 'react-i18next';
import {
  X,
  Paperclip,
  Eye,
  EyeOff,
  Check,
  Copy,
  ExternalLink,
  FolderOpen,
  Send,
  ShieldCheck,
  Loader2,
  Trash2,
} from 'lucide-react';
import { bridgeRequest } from '../hooks/useBridge';
import './FeedbackModal.css';

type FeedbackType = 'bug' | 'feature' | 'performance' | 'other';

interface FeedbackPreviewResponse {
  ok?: boolean;
  environment?: Record<string, unknown>;
  redactedMainLog?: string;
  redactedServiceLog?: string;
  markdownReport?: string;
  githubIssueUrl?: string;
  totalRedactions?: number;
  relayConfigured?: boolean;
}

interface FeedbackSubmitResponse {
  ok?: boolean;
  mode?: 'relay_submitted' | 'fallback_local';
  trackingId?: string;
  issueUrl?: string;
  exportedBundlePath?: string;
  markdownReport?: string;
  errorMessage?: string;
  totalRedactions?: number;
}

interface FeedbackModalProps {
  open: boolean;
  onClose: () => void;
}

export const FeedbackModal: FC<FeedbackModalProps> = ({ open, onClose }) => {
  const { t } = useTranslation();
  const [feedbackType, setFeedbackType] = useState<FeedbackType>('bug');
  const [title, setTitle] = useState('');
  const [description, setDescription] = useState('');
  const [stepsToReproduce, setStepsToReproduce] = useState('');
  const [contact, setContact] = useState('');
  const [includeLogs, setIncludeLogs] = useState(true);
  const [screenshotDataUri, setScreenshotDataUri] = useState('');
  const [screenshotName, setScreenshotName] = useState('');

  const [previewOpen, setPreviewOpen] = useState(false);
  const [previewLoading, setPreviewLoading] = useState(false);
  const [previewData, setPreviewData] = useState<FeedbackPreviewResponse | null>(null);

  const [submitting, setSubmitting] = useState(false);
  const [submitResult, setSubmitResult] = useState<FeedbackSubmitResponse | null>(null);
  const [copiedMarkdown, setCopiedMarkdown] = useState(false);

  const fileInputRef = useRef<HTMLInputElement>(null);

  useEffect(() => {
    if (!open) return;
    const onKeyDown = (e: KeyboardEvent) => {
      if (e.key === 'Escape') {
        e.preventDefault();
        onClose();
      }
    };
    window.addEventListener('keydown', onKeyDown);
    return () => window.removeEventListener('keydown', onKeyDown);
  }, [open, onClose]);

  const buildPayload = useCallback(
    () => ({
      feedbackType,
      title: title.trim(),
      description: description.trim(),
      stepsToReproduce: stepsToReproduce.trim(),
      contact: contact.trim(),
      includeLogs,
      screenshotDataUri,
    }),
    [feedbackType, title, description, stepsToReproduce, contact, includeLogs, screenshotDataUri],
  );

  const handleTogglePreview = useCallback(async () => {
    if (previewOpen) {
      setPreviewOpen(false);
      return;
    }
    setPreviewOpen(true);
    setPreviewLoading(true);
    try {
      const res = await bridgeRequest<FeedbackPreviewResponse>('feedback.preview', buildPayload(), { silent: true });
      if (res) {
        setPreviewData(res);
      }
    } finally {
      setPreviewLoading(false);
    }
  }, [previewOpen, buildPayload]);

  const handleFileChange = useCallback((e: ChangeEvent<HTMLInputElement>) => {
    const file = e.target.files?.[0];
    if (!file || file.size > 6 * 1024 * 1024) return;
    const reader = new FileReader();
    reader.onload = () => {
      if (typeof reader.result === 'string') {
        setScreenshotDataUri(reader.result);
        setScreenshotName(file.name);
      }
    };
    reader.readAsDataURL(file);
  }, []);

  const handlePaste = useCallback((e: ClipboardEvent<HTMLDivElement>) => {
    const items = e.clipboardData?.items;
    if (!items) return;
    for (let i = 0; i < items.length; i += 1) {
      const item = items[i];
      if (item.type.startsWith('image/')) {
        const blob = item.getAsFile();
        if (!blob || blob.size > 6 * 1024 * 1024) continue;
        const reader = new FileReader();
        reader.onload = () => {
          if (typeof reader.result === 'string') {
            setScreenshotDataUri(reader.result);
            setScreenshotName('clipboard-screenshot.png');
          }
        };
        reader.readAsDataURL(blob);
        break;
      }
    }
  }, []);

  const handleSubmit = useCallback(async () => {
    if (!description.trim() && !title.trim()) return;
    setSubmitting(true);
    try {
      const res = await bridgeRequest<FeedbackSubmitResponse>('feedback.submit', buildPayload(), { silent: true });
      if (res) {
        setSubmitResult(res);
      }
    } finally {
      setSubmitting(false);
    }
  }, [description, title, buildPayload]);

  const handleCopyMarkdown = useCallback(async (text: string) => {
    if (!text) return;
    try {
      await navigator.clipboard.writeText(text);
    } catch {
      await bridgeRequest('quicklook.copyPath', { path: text }, { silent: true }).catch(() => {});
    }
    setCopiedMarkdown(true);
    window.setTimeout(() => setCopiedMarkdown(false), 1800);
  }, []);

  if (!open) return null;

  const canSubmit = Boolean((title.trim() || description.trim()) && !submitting);

  return (
    <div className="fb-backdrop" role="dialog" aria-modal="true" aria-label={t('feedback.modalTitle', 'Give Feedback')} onPaste={handlePaste}>
      <div className="fb-panel">
        <header className="fb-header">
          <div className="fb-header-title">
            <h2>{t('feedback.modalTitle', 'Give Feedback')}</h2>
            <span className="fb-privacy-pill">
              <ShieldCheck size={14} aria-hidden="true" />
              <span>{t('feedback.privacyBadge', '10-Rule Local Privacy Redaction')}</span>
            </span>
          </div>
          <button
            type="button"
            className="fb-close-btn"
            onClick={onClose}
            aria-label={t('common.close', 'Close')}
          >
            <X size={18} aria-hidden="true" />
          </button>
        </header>

        <div className="fb-body">
          {submitResult ? (
            <div className="fb-result-card">
              <div className="fb-result-header">
                <Check size={20} className="fb-result-ok-icon" aria-hidden="true" />
                <div>
                  <div className="fb-result-title">
                    {submitResult.mode === 'relay_submitted'
                      ? t('feedback.submittedRelayTitle', 'Feedback Submitted Successfully')
                      : t('feedback.exportedLocalTitle', 'Redacted Diagnostic Bundle Ready')}
                  </div>
                  <div className="fb-result-sub">
                    {submitResult.trackingId
                      ? t('feedback.trackingIdLabel', 'Tracking ID: {{id}}', { id: submitResult.trackingId })
                      : t('feedback.redactionSummary', 'Automatically redacted {{count}} sensitive items locally.', {
                          count: submitResult.totalRedactions ?? 0,
                        })}
                  </div>
                </div>
              </div>

              {submitResult.exportedBundlePath && (
                <div className="fb-bundle-path-box">
                  <span className="fb-bundle-path-text">{submitResult.exportedBundlePath}</span>
                  <button
                    type="button"
                    className="fb-btn fb-btn-secondary"
                    onClick={() => void bridgeRequest('feedback.revealBundle', { path: submitResult.exportedBundlePath })}
                  >
                    <FolderOpen size={14} aria-hidden="true" />
                    <span>{t('feedback.revealZip', 'Show Bundle')}</span>
                  </button>
                </div>
              )}

              <div className="fb-result-actions">
                {submitResult.issueUrl && (
                  <button
                    type="button"
                    className="fb-btn fb-btn-primary"
                    onClick={() => void bridgeRequest('feedback.openExternalUrl', { url: submitResult.issueUrl })}
                  >
                    <ExternalLink size={14} aria-hidden="true" />
                    <span>
                      {submitResult.mode === 'relay_submitted'
                        ? t('feedback.viewIssue', 'View Issue')
                        : t('feedback.openPrefilledGitHub', 'Open Prefilled GitHub Issue')}
                    </span>
                  </button>
                )}
                {submitResult.markdownReport && (
                  <button
                    type="button"
                    className="fb-btn fb-btn-secondary"
                    onClick={() => void handleCopyMarkdown(submitResult.markdownReport || '')}
                  >
                    {copiedMarkdown ? <Check size={14} aria-hidden="true" /> : <Copy size={14} aria-hidden="true" />}
                    <span>
                      {copiedMarkdown
                        ? t('feedback.copiedReport', 'Copied')
                        : t('feedback.copyMarkdown', 'Copy Markdown Report')}
                    </span>
                  </button>
                )}
                <button
                  type="button"
                  className="fb-btn fb-btn-secondary"
                  onClick={() => setSubmitResult(null)}
                >
                  <span>{t('feedback.backToForm', 'Back to Edit')}</span>
                </button>
              </div>
            </div>
          ) : (
            <>
              <section className="fb-section">
                <div className="fb-label">{t('feedback.typeLabel', 'Feedback Type')}</div>
                <div className="fb-radio-list" role="radiogroup">
                  {(
                    [
                      ['bug', t('feedback.typeBug', 'Bug Report')],
                      ['feature', t('feedback.typeFeature', 'Feature Request')],
                      ['performance', t('feedback.typePerformance', 'Performance & Memory Issue')],
                      ['other', t('feedback.typeOther', 'General Feedback')],
                    ] as Array<[FeedbackType, string]>
                  ).map(([val, label]) => (
                    <label key={val} className={`fb-radio-item ${feedbackType === val ? 'active' : ''}`}>
                      <input
                        type="radio"
                        name="feedback-type"
                        checked={feedbackType === val}
                        onChange={() => setFeedbackType(val)}
                      />
                      <span>{label}</span>
                    </label>
                  ))}
                </div>
              </section>

              <section className="fb-section">
                <label className="fb-label" htmlFor="fb-title-input">
                  {t('feedback.titleLabel', 'Summary')}
                </label>
                <input
                  id="fb-title-input"
                  type="text"
                  className="fb-input"
                  value={title}
                  onChange={(e) => setTitle(e.target.value)}
                  placeholder={t('feedback.titlePlaceholder', 'Brief summary of the issue or idea...')}
                />
              </section>

              <section className="fb-section">
                <label className="fb-label" htmlFor="fb-desc-input">
                  {t('feedback.descriptionLabel', 'Description')}
                </label>
                <p className="fb-helper-text">
                  {t('feedback.descriptionHelper', 'Describe what happened, expected vs actual behavior, and any error details. You can press Ctrl+V anywhere to attach a screenshot.')}
                </p>
                <textarea
                  id="fb-desc-input"
                  className="fb-textarea"
                  rows={4}
                  value={description}
                  onChange={(e) => setDescription(e.target.value)}
                  placeholder={t('feedback.descriptionPlaceholder', 'Describe the issue or suggestion in detail...')}
                />
              </section>

              <section className="fb-section">
                <label className="fb-label" htmlFor="fb-steps-input">
                  {t('feedback.stepsLabel', 'Steps to Reproduce')}
                </label>
                <textarea
                  id="fb-steps-input"
                  className="fb-textarea"
                  rows={3}
                  value={stepsToReproduce}
                  onChange={(e) => setStepsToReproduce(e.target.value)}
                  placeholder={t('feedback.stepsPlaceholder', '1. Open ...\n2. Press ...\n3. Observe ...')}
                />
              </section>

              <section className="fb-section">
                <label className="fb-label" htmlFor="fb-contact-input">
                  {t('feedback.contactLabel', 'Contact (Optional)')}
                </label>
                <input
                  id="fb-contact-input"
                  type="text"
                  className="fb-input"
                  value={contact}
                  onChange={(e) => setContact(e.target.value)}
                  placeholder={t('feedback.contactPlaceholder', 'GitHub handle or community username (optional)')}
                />
              </section>

              <section className="fb-attach-row">
                <input
                  ref={fileInputRef}
                  type="file"
                  accept="image/png,image/jpeg,image/webp"
                  className="fb-hidden-file"
                  onChange={handleFileChange}
                />
                <button
                  type="button"
                  className="fb-link-btn"
                  onClick={() => fileInputRef.current?.click()}
                >
                  <Paperclip size={15} aria-hidden="true" />
                  <span>{t('feedback.attachScreenshot', 'Attach a screenshot (optional, or Ctrl+V)')}</span>
                </button>
                {screenshotDataUri && (
                  <div className="fb-screenshot-chip">
                    <img src={screenshotDataUri} alt={screenshotName} className="fb-screenshot-thumb" />
                    <span className="fb-screenshot-name">{screenshotName}</span>
                    <button
                      type="button"
                      className="fb-icon-btn"
                      onClick={() => {
                        setScreenshotDataUri('');
                        setScreenshotName('');
                      }}
                      aria-label={t('common.delete', 'Delete')}
                    >
                      <Trash2 size={14} aria-hidden="true" />
                    </button>
                  </div>
                )}
              </section>

              <section className="fb-logs-row">
                <label className="fb-checkbox-label">
                  <input
                    type="checkbox"
                    checked={includeLogs}
                    onChange={(e) => setIncludeLogs(e.target.checked)}
                  />
                  <span>{t('feedback.attachLogs', 'Attach redacted environment info and latest 200 log lines')}</span>
                </label>
                <button
                  type="button"
                  className="fb-link-btn"
                  onClick={() => void handleTogglePreview()}
                >
                  {previewOpen ? <EyeOff size={14} aria-hidden="true" /> : <Eye size={14} aria-hidden="true" />}
                  <span>
                    {previewOpen
                      ? t('feedback.hideRedactedPreview', 'Hide Preview')
                      : t('feedback.previewRedacted', 'Preview Redacted Data')}
                  </span>
                </button>
              </section>

              {previewOpen && (
                <div className="fb-preview-box">
                  {previewLoading ? (
                    <div className="fb-preview-loading">
                      <Loader2 size={16} className="fb-spin" aria-hidden="true" />
                      <span>{t('feedback.loadingPreview', 'Collecting and redacting diagnostics locally...')}</span>
                    </div>
                  ) : (
                    <>
                      <div className="fb-preview-meta">
                        <span>
                          {t('feedback.redactionSummary', 'Automatically redacted {{count}} sensitive items locally.', {
                            count: previewData?.totalRedactions ?? 0,
                          })}
                        </span>
                      </div>
                      <pre className="fb-preview-pre">
                        {previewData?.markdownReport || ''}
                      </pre>
                    </>
                  )}
                </div>
              )}
            </>
          )}
        </div>

        {!submitResult && (
          <footer className="fb-footer">
            <span className="fb-footer-note">
              {t('feedback.dualTrackNote', 'All personal paths, tokens, emails, IPs and SIDs are scrubbed locally before export or upload.')}
            </span>
            <div className="fb-footer-actions">
              <button type="button" className="fb-btn fb-btn-secondary" onClick={onClose}>
                {t('common.cancel', 'Cancel')}
              </button>
              <button
                type="button"
                className="fb-btn fb-btn-primary"
                disabled={!canSubmit}
                onClick={() => void handleSubmit()}
              >
                {submitting ? (
                  <Loader2 size={14} className="fb-spin" aria-hidden="true" />
                ) : (
                  <Send size={14} aria-hidden="true" />
                )}
                <span>{t('feedback.submit', 'Submit / Export Bundle')}</span>
              </button>
            </div>
          </footer>
        )}
      </div>
    </div>
  );
};
