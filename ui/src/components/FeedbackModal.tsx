import {
  useState,
  useEffect,
  useCallback,
  useRef,
  type FC,
  type ChangeEvent,
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
  AlertCircle,
} from 'lucide-react';
import { toast } from 'sonner';
import { bridgeRequest } from '../hooks/useBridge';
import './FeedbackModal.css';

const MAX_FEEDBACK_SCREENSHOT_BYTES = 6 * 1024 * 1024;

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
  exportedScreenshotPath?: string;
  copiedToClipboard?: boolean;
  markdownReport?: string;
  errorMessage?: string;
  totalRedactions?: number;
}

interface FeedbackClipboardImageResponse {
  ok?: boolean;
  tooLarge?: boolean;
  dataUri?: string;
  fileName?: string;
  byteSize?: number;
}

interface FeedbackModalProps {
  open: boolean;
  onClose: () => void;
}

function isEditableElement(el: EventTarget | null): boolean {
  if (!(el instanceof HTMLElement)) return false;
  const tag = el.tagName;
  return tag === 'INPUT' || tag === 'TEXTAREA' || el.isContentEditable;
}

function isSupportedImageFile(file: File): boolean {
  if (file.type && file.type.startsWith('image/')) return true;
  return /\.(png|jpe?g|webp|bmp|gif)$/i.test(file.name);
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
  const [errorMessage, setErrorMessage] = useState('');
  const [copiedMarkdown, setCopiedMarkdown] = useState(false);

  const fileInputRef = useRef<HTMLInputElement>(null);
  const panelRef = useRef<HTMLDivElement>(null);
  const lastPasteHandledAtRef = useRef<number>(0);

  const onCloseRef = useRef(onClose);
  useEffect(() => {
    onCloseRef.current = onClose;
  }, [onClose]);

  const handleClose = useCallback(() => {
    setErrorMessage('');
    onCloseRef.current();
  }, []);

  const attachImageBlob = useCallback((blob: Blob, preferredName: string, showToast: boolean) => {
    if (blob.size > MAX_FEEDBACK_SCREENSHOT_BYTES) {
      const msg = t('feedback.screenshotTooLarge', 'Screenshot exceeds the 6 MB limit. Please choose a smaller image.');
      setErrorMessage(msg);
      toast.error(msg);
      return;
    }
    setErrorMessage('');
    const reader = new FileReader();
    reader.onload = () => {
      if (typeof reader.result === 'string') {
        setScreenshotDataUri(reader.result);
        setScreenshotName(preferredName || 'clipboard-screenshot.png');
        if (showToast) {
          toast.success(t('feedback.screenshotAttached', 'Screenshot attached from clipboard.'));
        }
      }
    };
    reader.readAsDataURL(blob);
  }, [t]);

  const tryNativeClipboardImagePaste = useCallback(async () => {
    try {
      const res = await bridgeRequest<FeedbackClipboardImageResponse>(
        'feedback.readClipboardImage',
        {},
        { silent: true },
      );
      if (res?.tooLarge) {
        const msg = t('feedback.screenshotTooLarge', 'Screenshot exceeds the 6 MB limit. Please choose a smaller image.');
        setErrorMessage(msg);
        toast.error(msg);
        return true;
      }
      if (res?.ok && res.dataUri) {
        setErrorMessage('');
        setScreenshotDataUri(res.dataUri);
        setScreenshotName(res.fileName || 'clipboard-screenshot.png');
        toast.success(t('feedback.screenshotAttached', 'Screenshot attached from clipboard.'));
        return true;
      }
    } catch {
      // Ignore native clipboard probe errors when clipboard has no image
    }
    return false;
  }, [t]);

  useEffect(() => {
    if (!open) return;
    const frame = requestAnimationFrame(() => {
      const titleEl = document.getElementById('fb-title-input') as HTMLElement | null;
      (titleEl ?? panelRef.current)?.focus();
    });
    return () => cancelAnimationFrame(frame);
  }, [open]);

  useEffect(() => {
    if (!open) return;

    let nativePasteTimer: number | null = null;
    const onKeyDown = (e: KeyboardEvent) => {
      if (e.isComposing || e.keyCode === 229 || e.key === 'Process') return;
      if (e.key === 'Escape') {
        e.preventDefault();
        handleClose();
        return;
      }
      if (!submitResult && (e.ctrlKey || e.metaKey) && !e.altKey && e.key.toLowerCase() === 'v') {
        if (!isEditableElement(e.target)) {
          // 非文本编辑焦点下 WebView2 可能不派发 DOM paste 事件：短延迟后由 Win32 原生剪贴板兜底捞图
          if (nativePasteTimer !== null) window.clearTimeout(nativePasteTimer);
          nativePasteTimer = window.setTimeout(() => {
            if (Date.now() - lastPasteHandledAtRef.current > 180) {
              lastPasteHandledAtRef.current = Date.now();
              void tryNativeClipboardImagePaste();
            }
          }, 35);
        }
      }
    };

    const onWindowPaste = (e: globalThis.ClipboardEvent) => {
      if (submitResult) return;
      const dt = e.clipboardData;
      if (dt) {
        const items = dt.items;
        if (items) {
          for (let i = 0; i < items.length; i += 1) {
            const item = items[i];
            if (item.type.startsWith('image/')) {
              const blob = item.getAsFile();
              if (blob) {
                e.preventDefault();
                lastPasteHandledAtRef.current = Date.now();
                attachImageBlob(blob, blob.name || 'clipboard-screenshot.png', true);
                return;
              }
            }
          }
        }
        const files = dt.files;
        if (files && files.length > 0) {
          for (let i = 0; i < files.length; i += 1) {
            const file = files[i];
            if (isSupportedImageFile(file)) {
              e.preventDefault();
              lastPasteHandledAtRef.current = Date.now();
              attachImageBlob(file, file.name || 'clipboard-screenshot.png', true);
              return;
            }
          }
        }
        // 若用户正在输入框粘贴纯文本，100% 放行原生文字粘贴不干扰
        const plainText = dt.getData('text/plain');
        if (plainText && isEditableElement(e.target)) {
          return;
        }
      }

      // DOM clipboardData 未暴露图片（常见于 WebView2 下 Win32 CF_BITMAP / CF_HDROP 截图），走原生剪贴板兜底
      if (Date.now() - lastPasteHandledAtRef.current > 180) {
        lastPasteHandledAtRef.current = Date.now();
        void tryNativeClipboardImagePaste();
      }
    };

    window.addEventListener('keydown', onKeyDown);
    window.addEventListener('paste', onWindowPaste);
    return () => {
      if (nativePasteTimer !== null) window.clearTimeout(nativePasteTimer);
      window.removeEventListener('keydown', onKeyDown);
      window.removeEventListener('paste', onWindowPaste);
    };
  }, [open, submitResult, handleClose, attachImageBlob, tryNativeClipboardImagePaste]);

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
    setErrorMessage('');
    setPreviewOpen(true);
    setPreviewLoading(true);
    try {
      const res = await bridgeRequest<FeedbackPreviewResponse>('feedback.preview', buildPayload(), { silent: true });
      if (res && res.ok !== false) {
        setPreviewData(res);
      } else {
        setPreviewOpen(false);
        const msg = t('feedback.previewFailed', 'Failed to collect redacted diagnostic preview.');
        setErrorMessage(msg);
        toast.error(msg);
      }
    } catch (error) {
      setPreviewOpen(false);
      const detail = error instanceof Error && error.message ? error.message : '';
      const msg = t('feedback.previewFailed', 'Failed to collect redacted diagnostic preview.');
      setErrorMessage(detail ? `${msg} (${detail})` : msg);
      toast.error(msg, detail ? { description: detail } : undefined);
    } finally {
      setPreviewLoading(false);
    }
  }, [previewOpen, buildPayload, t]);

  const handleFileChange = useCallback((e: ChangeEvent<HTMLInputElement>) => {
    const file = e.target.files?.[0];
    e.target.value = '';
    if (!file) return;
    attachImageBlob(file, file.name, false);
  }, [attachImageBlob]);

  const handleSubmit = useCallback(async () => {
    if (!description.trim() && !title.trim()) return;
    setErrorMessage('');
    setSubmitting(true);
    try {
      const res = await bridgeRequest<FeedbackSubmitResponse>('feedback.submit', buildPayload(), { silent: true });
      if (res && res.ok !== false) {
        setSubmitResult(res);
        toast.success(
          res.mode === 'relay_submitted'
            ? t('feedback.submittedRelayTitle', 'Feedback Submitted Successfully')
            : t('feedback.exportedLocalTitle', 'Redacted Diagnostic Bundle Ready'),
        );
      } else {
        const fallbackMsg = t('feedback.submitFailed', 'Failed to submit feedback or export diagnostic bundle.');
        const detail = res?.errorMessage || '';
        setErrorMessage(detail ? `${fallbackMsg} (${detail})` : fallbackMsg);
        toast.error(fallbackMsg, detail ? { description: detail } : undefined);
      }
    } catch (error) {
      const fallbackMsg = t('feedback.submitFailed', 'Failed to submit feedback or export diagnostic bundle.');
      const detail = error instanceof Error && error.message ? error.message : '';
      setErrorMessage(detail ? `${fallbackMsg} (${detail})` : fallbackMsg);
      toast.error(fallbackMsg, detail ? { description: detail } : undefined);
    } finally {
      setSubmitting(false);
    }
  }, [description, title, buildPayload, t]);

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
    <div className="fb-backdrop" role="dialog" aria-modal="true" aria-label={t('feedback.modalTitle', 'Give Feedback')}>
      <div className="fb-panel" ref={panelRef} tabIndex={-1}>
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
            onClick={handleClose}
            aria-label={t('common.close', 'Close')}
          >
            <X size={18} aria-hidden="true" />
          </button>
        </header>

        <div className="fb-body">
          {errorMessage && (
            <div className="fb-error-banner" role="alert">
              <AlertCircle size={16} aria-hidden="true" />
              <span>{errorMessage}</span>
            </div>
          )}
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
                <div className="fb-paste-guide-banner" role="status">
                  <Paperclip size={15} strokeWidth={2.2} aria-hidden="true" />
                  <span>
                    {t(
                      'feedback.pasteBundleBanner',
                      'The redacted diagnostic .zip (and screenshot) has been copied to your clipboard - just press Ctrl+V inside the GitHub Issue editor to upload them in one keystroke!',
                    )}
                  </span>
                </div>
              )}

              {submitResult.exportedBundlePath && (
                <div className="fb-bundle-path-box">
                  <span className="fb-bundle-path-text">{submitResult.exportedBundlePath}</span>
                  <div className="fb-bundle-path-btns">
                    <button
                      type="button"
                      className="fb-btn fb-btn-secondary"
                      onClick={async () => {
                        await bridgeRequest(
                          'feedback.copyBundleFiles',
                          {
                            bundlePath: submitResult.exportedBundlePath,
                            screenshotPath: submitResult.exportedScreenshotPath || '',
                          },
                          { silent: true },
                        );
                        toast.success(
                          t(
                            'feedback.bundleFilesCopiedToast',
                            'Diagnostic bundle copied to clipboard - press Ctrl+V in GitHub to upload',
                          ),
                        );
                      }}
                    >
                      <Copy size={14} aria-hidden="true" />
                      <span>{t('feedback.copyBundleFiles', 'Copy Bundle File')}</span>
                    </button>
                    <button
                      type="button"
                      className="fb-btn fb-btn-secondary"
                      onClick={() => void bridgeRequest('feedback.revealBundle', { path: submitResult.exportedBundlePath })}
                    >
                      <FolderOpen size={14} aria-hidden="true" />
                      <span>{t('feedback.revealZip', 'Show Bundle')}</span>
                    </button>
                  </div>
                </div>
              )}

              <div className="fb-result-actions">
                {submitResult.issueUrl && (
                  <button
                    type="button"
                    className="fb-btn fb-btn-primary"
                    onClick={() =>
                      void bridgeRequest('feedback.openExternalUrl', {
                        url: submitResult.issueUrl,
                        bundlePath: submitResult.exportedBundlePath || '',
                        screenshotPath: submitResult.exportedScreenshotPath || '',
                      })
                    }
                  >
                    <ExternalLink size={14} aria-hidden="true" />
                    <span>
                      {submitResult.mode === 'relay_submitted'
                        ? t('feedback.viewIssue', 'View Issue')
                        : submitResult.exportedBundlePath
                          ? t('feedback.openGitHubAndCopyBundle', 'Open GitHub Issue (Bundle Copied)')
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
                  <span>{t('feedback.attachLogs', 'Attach redacted environment info and latest 1000 log lines')}</span>
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
              <button type="button" className="fb-btn fb-btn-secondary" onClick={handleClose}>
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
