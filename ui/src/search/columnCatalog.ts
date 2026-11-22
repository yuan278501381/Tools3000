import type {
  PropertyCellValue,
  SearchResult,
  SortDirection,
  TableColumnId,
} from './searchTypes';

export type PropertyCategoryId =
  | 'all'
  | 'file'
  | 'image'
  | 'audio'
  | 'video'
  | 'document'
  | 'program'
  | 'hash'
  | 'windows';

export type ConcretePropertyCategoryId = Exclude<PropertyCategoryId, 'all'>;

export interface TableColumnDefinition {
  id: TableColumnId;
  tier: 1 | 2;
  category: ConcretePropertyCategoryId;
  labelKey: string;
  defaultLabel: string;
  canonicalName: string;
  minWidth: number;
  defaultWidth: number;
  maxWidth: number;
  isFlex: boolean;
  align: 'left' | 'right';
  isMono?: boolean;
  locked?: boolean;
  /** 是否需要调用后端 `search.batchGetProperties` 惰性提取。 */
  requiresBackendProp: boolean;
  defaultSortDir: SortDirection;
  /** 排序时主比较类型 */
  sortKind: 'text' | 'number' | 'date';
}

export interface PropertyCategoryMeta {
  id: PropertyCategoryId;
  labelKey: string;
  defaultLabel: string;
}

export const PROPERTY_CATEGORIES: readonly PropertyCategoryMeta[] = [
  { id: 'all', labelKey: 'search.propCatAll', defaultLabel: 'All Properties' },
  { id: 'file', labelKey: 'search.propCatFile', defaultLabel: 'File' },
  { id: 'image', labelKey: 'search.propCatImage', defaultLabel: 'Image' },
  { id: 'audio', labelKey: 'search.propCatAudio', defaultLabel: 'Audio' },
  { id: 'video', labelKey: 'search.propCatVideo', defaultLabel: 'Video' },
  { id: 'document', labelKey: 'search.propCatDocument', defaultLabel: 'Document' },
  { id: 'program', labelKey: 'search.propCatProgram', defaultLabel: 'Program' },
  { id: 'hash', labelKey: 'search.propCatHash', defaultLabel: 'Hash & Index' },
  { id: 'windows', labelKey: 'search.propCatWindows', defaultLabel: 'Windows Property System' },
];

/** 默认开箱表格列（保持与既有 5 列完全一致，确保用户无感平滑升级） */
export const DEFAULT_VISIBLE_TABLE_COLUMNS: readonly TableColumnId[] = [
  'name',
  'path',
  'size',
  'modified',
  'created',
];

/** 右键表头快捷勾选的 10 个核心基础列（Everything 1.5 同款 Tier 1 核心清单） */
export const TIER1_QUICK_COLUMN_IDS: readonly TableColumnId[] = [
  'name',
  'path',
  'size',
  'ext',
  'type',
  'modified',
  'created',
  'accessed',
  'attributes',
  'runCount',
];

export const BUILTIN_TABLE_COLUMNS: readonly TableColumnDefinition[] = [
  // ── Tier 1: 10 核心基础列 ───────────────────────────────────────────────────
  {
    id: 'name',
    tier: 1,
    category: 'file',
    labelKey: 'search.colName',
    defaultLabel: 'File Name',
    canonicalName: 'Name',
    minWidth: 120,
    defaultWidth: 320,
    maxWidth: 960,
    isFlex: true,
    align: 'left',
    locked: true,
    requiresBackendProp: false,
    defaultSortDir: 'asc',
    sortKind: 'text',
  },
  {
    id: 'path',
    tier: 1,
    category: 'file',
    labelKey: 'search.colDirectory',
    defaultLabel: 'Directory',
    canonicalName: 'Path',
    minWidth: 140,
    defaultWidth: 460,
    maxWidth: 1600,
    isFlex: true,
    align: 'left',
    requiresBackendProp: false,
    defaultSortDir: 'asc',
    sortKind: 'text',
  },
  {
    id: 'size',
    tier: 1,
    category: 'file',
    labelKey: 'search.colSizeShort',
    defaultLabel: 'Size',
    canonicalName: 'Size',
    minWidth: 72,
    defaultWidth: 96,
    maxWidth: 280,
    isFlex: false,
    align: 'right',
    isMono: true,
    requiresBackendProp: false,
    defaultSortDir: 'desc',
    sortKind: 'number',
  },
  {
    id: 'ext',
    tier: 1,
    category: 'file',
    labelKey: 'search.colExtension',
    defaultLabel: 'Extension',
    canonicalName: 'Extension',
    minWidth: 68,
    defaultWidth: 88,
    maxWidth: 200,
    isFlex: false,
    align: 'left',
    isMono: true,
    requiresBackendProp: false,
    defaultSortDir: 'asc',
    sortKind: 'text',
  },
  {
    id: 'type',
    tier: 1,
    category: 'file',
    labelKey: 'search.colFileType',
    defaultLabel: 'Type',
    canonicalName: 'Type',
    minWidth: 96,
    defaultWidth: 150,
    maxWidth: 360,
    isFlex: false,
    align: 'left',
    requiresBackendProp: true,
    defaultSortDir: 'asc',
    sortKind: 'text',
  },
  {
    id: 'modified',
    tier: 1,
    category: 'file',
    labelKey: 'search.colModified',
    defaultLabel: 'Modified Time',
    canonicalName: 'Date Modified',
    minWidth: 118,
    defaultWidth: 152,
    maxWidth: 320,
    isFlex: false,
    align: 'left',
    isMono: true,
    requiresBackendProp: false,
    defaultSortDir: 'desc',
    sortKind: 'date',
  },
  {
    id: 'created',
    tier: 1,
    category: 'file',
    labelKey: 'search.colCreated',
    defaultLabel: 'Created Time',
    canonicalName: 'Date Created',
    minWidth: 118,
    defaultWidth: 152,
    maxWidth: 320,
    isFlex: false,
    align: 'left',
    isMono: true,
    requiresBackendProp: false,
    defaultSortDir: 'desc',
    sortKind: 'date',
  },
  {
    id: 'accessed',
    tier: 1,
    category: 'file',
    labelKey: 'search.colAccessed',
    defaultLabel: 'Date Accessed',
    canonicalName: 'Date Accessed',
    minWidth: 118,
    defaultWidth: 152,
    maxWidth: 320,
    isFlex: false,
    align: 'left',
    isMono: true,
    requiresBackendProp: false,
    defaultSortDir: 'desc',
    sortKind: 'date',
  },
  {
    id: 'attributes',
    tier: 1,
    category: 'file',
    labelKey: 'search.colAttributes',
    defaultLabel: 'Attributes',
    canonicalName: 'Attributes',
    minWidth: 72,
    defaultWidth: 92,
    maxWidth: 200,
    isFlex: false,
    align: 'left',
    isMono: true,
    requiresBackendProp: false,
    defaultSortDir: 'asc',
    sortKind: 'text',
  },
  {
    id: 'runCount',
    tier: 1,
    category: 'file',
    labelKey: 'search.colRunCount',
    defaultLabel: 'Run Count',
    canonicalName: 'Run Count',
    minWidth: 76,
    defaultWidth: 92,
    maxWidth: 180,
    isFlex: false,
    align: 'right',
    isMono: true,
    requiresBackendProp: false,
    defaultSortDir: 'desc',
    sortKind: 'number',
  },

  // ── Tier 2: 文件扩展属性 ────────────────────────────────────────────────────
  {
    id: 'stem',
    tier: 2,
    category: 'file',
    labelKey: 'search.colStem',
    defaultLabel: 'Stem',
    canonicalName: 'Stem',
    minWidth: 100,
    defaultWidth: 180,
    maxWidth: 480,
    isFlex: false,
    align: 'left',
    requiresBackendProp: false,
    defaultSortDir: 'asc',
    sortKind: 'text',
  },
  {
    id: 'parentName',
    tier: 2,
    category: 'file',
    labelKey: 'search.colParentName',
    defaultLabel: 'Parent Name',
    canonicalName: 'Parent Name',
    minWidth: 96,
    defaultWidth: 160,
    maxWidth: 400,
    isFlex: false,
    align: 'left',
    requiresBackendProp: false,
    defaultSortDir: 'asc',
    sortKind: 'text',
  },
  {
    id: 'drive',
    tier: 2,
    category: 'file',
    labelKey: 'search.colDrive',
    defaultLabel: 'Drive',
    canonicalName: 'Drive',
    minWidth: 64,
    defaultWidth: 74,
    maxWidth: 140,
    isFlex: false,
    align: 'left',
    isMono: true,
    requiresBackendProp: false,
    defaultSortDir: 'asc',
    sortKind: 'text',
  },
  {
    id: 'fullPathLength',
    tier: 2,
    category: 'file',
    labelKey: 'search.colFullPathLength',
    defaultLabel: 'Full Path Length',
    canonicalName: 'Full Path Length',
    minWidth: 82,
    defaultWidth: 106,
    maxWidth: 180,
    isFlex: false,
    align: 'right',
    isMono: true,
    requiresBackendProp: false,
    defaultSortDir: 'desc',
    sortKind: 'number',
  },
  {
    id: 'nameLength',
    tier: 2,
    category: 'file',
    labelKey: 'search.colNameLength',
    defaultLabel: 'Name Length',
    canonicalName: 'Name Length',
    minWidth: 78,
    defaultWidth: 96,
    maxWidth: 180,
    isFlex: false,
    align: 'right',
    isMono: true,
    requiresBackendProp: false,
    defaultSortDir: 'desc',
    sortKind: 'number',
  },
  {
    id: 'owner',
    tier: 2,
    category: 'file',
    labelKey: 'search.colOwner',
    defaultLabel: 'Owner',
    canonicalName: 'Owner',
    minWidth: 110,
    defaultWidth: 170,
    maxWidth: 360,
    isFlex: false,
    align: 'left',
    requiresBackendProp: true,
    defaultSortDir: 'asc',
    sortKind: 'text',
  },
  {
    id: 'childCount',
    tier: 2,
    category: 'hash',
    labelKey: 'search.colChildCount',
    defaultLabel: 'Child Count',
    canonicalName: 'Child Count',
    minWidth: 82,
    defaultWidth: 102,
    maxWidth: 180,
    isFlex: false,
    align: 'right',
    isMono: true,
    requiresBackendProp: true,
    defaultSortDir: 'desc',
    sortKind: 'number',
  },

  // ── Tier 2: 图片属性 (WIC 零解码读头 + EXIF Property System) ────────────────
  {
    id: 'dimensions',
    tier: 2,
    category: 'image',
    labelKey: 'search.colDimensions',
    defaultLabel: 'Dimensions',
    canonicalName: 'Dimensions',
    minWidth: 104,
    defaultWidth: 132,
    maxWidth: 260,
    isFlex: false,
    align: 'right',
    isMono: true,
    requiresBackendProp: true,
    defaultSortDir: 'desc',
    sortKind: 'number',
  },
  {
    id: 'width',
    tier: 2,
    category: 'image',
    labelKey: 'search.colWidth',
    defaultLabel: 'Width',
    canonicalName: 'Width',
    minWidth: 76,
    defaultWidth: 92,
    maxWidth: 180,
    isFlex: false,
    align: 'right',
    isMono: true,
    requiresBackendProp: true,
    defaultSortDir: 'desc',
    sortKind: 'number',
  },
  {
    id: 'height',
    tier: 2,
    category: 'image',
    labelKey: 'search.colHeight',
    defaultLabel: 'Height',
    canonicalName: 'Height',
    minWidth: 76,
    defaultWidth: 92,
    maxWidth: 180,
    isFlex: false,
    align: 'right',
    isMono: true,
    requiresBackendProp: true,
    defaultSortDir: 'desc',
    sortKind: 'number',
  },
  {
    id: 'aspectRatio',
    tier: 2,
    category: 'image',
    labelKey: 'search.colAspectRatio',
    defaultLabel: 'Aspect Ratio',
    canonicalName: 'Aspect Ratio',
    minWidth: 84,
    defaultWidth: 102,
    maxWidth: 180,
    isFlex: false,
    align: 'right',
    isMono: true,
    requiresBackendProp: true,
    defaultSortDir: 'desc',
    sortKind: 'number',
  },
  {
    id: 'bitDepth',
    tier: 2,
    category: 'image',
    labelKey: 'search.colBitDepth',
    defaultLabel: 'Bit Depth',
    canonicalName: 'Bit Depth',
    minWidth: 76,
    defaultWidth: 92,
    maxWidth: 160,
    isFlex: false,
    align: 'right',
    isMono: true,
    requiresBackendProp: true,
    defaultSortDir: 'desc',
    sortKind: 'number',
  },
  {
    id: 'dateTaken',
    tier: 2,
    category: 'image',
    labelKey: 'search.colDateTaken',
    defaultLabel: 'Date Taken',
    canonicalName: 'System.Photo.DateTaken',
    minWidth: 118,
    defaultWidth: 152,
    maxWidth: 280,
    isFlex: false,
    align: 'left',
    isMono: true,
    requiresBackendProp: true,
    defaultSortDir: 'desc',
    sortKind: 'date',
  },
  {
    id: 'cameraMaker',
    tier: 2,
    category: 'image',
    labelKey: 'search.colCameraMaker',
    defaultLabel: 'Camera Maker',
    canonicalName: 'System.Photo.CameraManufacturer',
    minWidth: 104,
    defaultWidth: 140,
    maxWidth: 280,
    isFlex: false,
    align: 'left',
    requiresBackendProp: true,
    defaultSortDir: 'asc',
    sortKind: 'text',
  },
  {
    id: 'cameraModel',
    tier: 2,
    category: 'image',
    labelKey: 'search.colCameraModel',
    defaultLabel: 'Camera Model',
    canonicalName: 'System.Photo.CameraModel',
    minWidth: 108,
    defaultWidth: 160,
    maxWidth: 320,
    isFlex: false,
    align: 'left',
    requiresBackendProp: true,
    defaultSortDir: 'asc',
    sortKind: 'text',
  },
  {
    id: 'focalLength',
    tier: 2,
    category: 'image',
    labelKey: 'search.colFocalLength',
    defaultLabel: 'Focal Length',
    canonicalName: 'System.Photo.FocalLength',
    minWidth: 84,
    defaultWidth: 102,
    maxWidth: 180,
    isFlex: false,
    align: 'right',
    isMono: true,
    requiresBackendProp: true,
    defaultSortDir: 'desc',
    sortKind: 'number',
  },
  {
    id: 'isoSpeed',
    tier: 2,
    category: 'image',
    labelKey: 'search.colIsoSpeed',
    defaultLabel: 'ISO Speed',
    canonicalName: 'System.Photo.ISOSpeed',
    minWidth: 82,
    defaultWidth: 98,
    maxWidth: 180,
    isFlex: false,
    align: 'right',
    isMono: true,
    requiresBackendProp: true,
    defaultSortDir: 'desc',
    sortKind: 'number',
  },

  // ── Tier 2: 音频属性 ────────────────────────────────────────────────────────
  {
    id: 'duration',
    tier: 2,
    category: 'audio',
    labelKey: 'search.colDuration',
    defaultLabel: 'Length',
    canonicalName: 'System.Media.Duration',
    minWidth: 82,
    defaultWidth: 98,
    maxWidth: 180,
    isFlex: false,
    align: 'right',
    isMono: true,
    requiresBackendProp: true,
    defaultSortDir: 'desc',
    sortKind: 'number',
  },
  {
    id: 'audioBitRate',
    tier: 2,
    category: 'audio',
    labelKey: 'search.colAudioBitRate',
    defaultLabel: 'Audio Bit Rate',
    canonicalName: 'System.Audio.EncodingBitrate',
    minWidth: 96,
    defaultWidth: 116,
    maxWidth: 200,
    isFlex: false,
    align: 'right',
    isMono: true,
    requiresBackendProp: true,
    defaultSortDir: 'desc',
    sortKind: 'number',
  },
  {
    id: 'audioSampleRate',
    tier: 2,
    category: 'audio',
    labelKey: 'search.colAudioSampleRate',
    defaultLabel: 'Audio Sample Rate',
    canonicalName: 'System.Audio.SampleRate',
    minWidth: 98,
    defaultWidth: 120,
    maxWidth: 200,
    isFlex: false,
    align: 'right',
    isMono: true,
    requiresBackendProp: true,
    defaultSortDir: 'desc',
    sortKind: 'number',
  },
  {
    id: 'audioChannels',
    tier: 2,
    category: 'audio',
    labelKey: 'search.colAudioChannels',
    defaultLabel: 'Audio Channels',
    canonicalName: 'System.Audio.ChannelCount',
    minWidth: 84,
    defaultWidth: 102,
    maxWidth: 180,
    isFlex: false,
    align: 'right',
    isMono: true,
    requiresBackendProp: true,
    defaultSortDir: 'desc',
    sortKind: 'number',
  },
  {
    id: 'artist',
    tier: 2,
    category: 'audio',
    labelKey: 'search.colArtist',
    defaultLabel: 'Artist',
    canonicalName: 'System.Music.Artist',
    minWidth: 100,
    defaultWidth: 150,
    maxWidth: 340,
    isFlex: false,
    align: 'left',
    requiresBackendProp: true,
    defaultSortDir: 'asc',
    sortKind: 'text',
  },
  {
    id: 'album',
    tier: 2,
    category: 'audio',
    labelKey: 'search.colAlbum',
    defaultLabel: 'Album',
    canonicalName: 'System.Music.AlbumTitle',
    minWidth: 100,
    defaultWidth: 160,
    maxWidth: 340,
    isFlex: false,
    align: 'left',
    requiresBackendProp: true,
    defaultSortDir: 'asc',
    sortKind: 'text',
  },
  {
    id: 'albumArtist',
    tier: 2,
    category: 'audio',
    labelKey: 'search.colAlbumArtist',
    defaultLabel: 'Album Artist',
    canonicalName: 'System.Music.AlbumArtist',
    minWidth: 104,
    defaultWidth: 150,
    maxWidth: 340,
    isFlex: false,
    align: 'left',
    requiresBackendProp: true,
    defaultSortDir: 'asc',
    sortKind: 'text',
  },
  {
    id: 'genre',
    tier: 2,
    category: 'audio',
    labelKey: 'search.colGenre',
    defaultLabel: 'Genre',
    canonicalName: 'System.Music.Genre',
    minWidth: 88,
    defaultWidth: 120,
    maxWidth: 260,
    isFlex: false,
    align: 'left',
    requiresBackendProp: true,
    defaultSortDir: 'asc',
    sortKind: 'text',
  },

  // ── Tier 2: 视频属性 ────────────────────────────────────────────────────────
  {
    id: 'videoDimensions',
    tier: 2,
    category: 'video',
    labelKey: 'search.colVideoDimensions',
    defaultLabel: 'Video Dimensions',
    canonicalName: 'System.Video.FrameWidth/Height',
    minWidth: 108,
    defaultWidth: 136,
    maxWidth: 260,
    isFlex: false,
    align: 'right',
    isMono: true,
    requiresBackendProp: true,
    defaultSortDir: 'desc',
    sortKind: 'number',
  },
  {
    id: 'frameRate',
    tier: 2,
    category: 'video',
    labelKey: 'search.colFrameRate',
    defaultLabel: 'Frame Rate',
    canonicalName: 'System.Video.FrameRate',
    minWidth: 88,
    defaultWidth: 108,
    maxWidth: 180,
    isFlex: false,
    align: 'right',
    isMono: true,
    requiresBackendProp: true,
    defaultSortDir: 'desc',
    sortKind: 'number',
  },

  // ── Tier 2: 文档属性 ────────────────────────────────────────────────────────
  {
    id: 'title',
    tier: 2,
    category: 'document',
    labelKey: 'search.colDocTitle',
    defaultLabel: 'Title',
    canonicalName: 'System.Title',
    minWidth: 110,
    defaultWidth: 180,
    maxWidth: 420,
    isFlex: false,
    align: 'left',
    requiresBackendProp: true,
    defaultSortDir: 'asc',
    sortKind: 'text',
  },
  {
    id: 'authors',
    tier: 2,
    category: 'document',
    labelKey: 'search.colAuthors',
    defaultLabel: 'Authors',
    canonicalName: 'System.Author',
    minWidth: 100,
    defaultWidth: 150,
    maxWidth: 340,
    isFlex: false,
    align: 'left',
    requiresBackendProp: true,
    defaultSortDir: 'asc',
    sortKind: 'text',
  },
  {
    id: 'subject',
    tier: 2,
    category: 'document',
    labelKey: 'search.colSubject',
    defaultLabel: 'Subject',
    canonicalName: 'System.Subject',
    minWidth: 100,
    defaultWidth: 160,
    maxWidth: 360,
    isFlex: false,
    align: 'left',
    requiresBackendProp: true,
    defaultSortDir: 'asc',
    sortKind: 'text',
  },
  {
    id: 'pageCount',
    tier: 2,
    category: 'document',
    labelKey: 'search.colPageCount',
    defaultLabel: 'Pages',
    canonicalName: 'System.Document.PageCount',
    minWidth: 74,
    defaultWidth: 88,
    maxWidth: 160,
    isFlex: false,
    align: 'right',
    isMono: true,
    requiresBackendProp: true,
    defaultSortDir: 'desc',
    sortKind: 'number',
  },
  {
    id: 'wordCount',
    tier: 2,
    category: 'document',
    labelKey: 'search.colWordCount',
    defaultLabel: 'Word Count',
    canonicalName: 'System.Document.WordCount',
    minWidth: 82,
    defaultWidth: 100,
    maxWidth: 180,
    isFlex: false,
    align: 'right',
    isMono: true,
    requiresBackendProp: true,
    defaultSortDir: 'desc',
    sortKind: 'number',
  },
  {
    id: 'comment',
    tier: 2,
    category: 'document',
    labelKey: 'search.colComment',
    defaultLabel: 'Comment',
    canonicalName: 'System.Comment',
    minWidth: 110,
    defaultWidth: 180,
    maxWidth: 420,
    isFlex: false,
    align: 'left',
    requiresBackendProp: true,
    defaultSortDir: 'asc',
    sortKind: 'text',
  },
  {
    id: 'rating',
    tier: 2,
    category: 'document',
    labelKey: 'search.colRating',
    defaultLabel: 'Rating',
    canonicalName: 'System.Rating',
    minWidth: 76,
    defaultWidth: 92,
    maxWidth: 160,
    isFlex: false,
    align: 'right',
    isMono: true,
    requiresBackendProp: true,
    defaultSortDir: 'desc',
    sortKind: 'number',
  },

  // ── Tier 2: 程序属性 (PE VersionInfo) ───────────────────────────────────────
  {
    id: 'fileVersion',
    tier: 2,
    category: 'program',
    labelKey: 'search.colFileVersion',
    defaultLabel: 'File Version',
    canonicalName: 'File Version',
    minWidth: 96,
    defaultWidth: 126,
    maxWidth: 260,
    isFlex: false,
    align: 'left',
    isMono: true,
    requiresBackendProp: true,
    defaultSortDir: 'desc',
    sortKind: 'text',
  },
  {
    id: 'productVersion',
    tier: 2,
    category: 'program',
    labelKey: 'search.colProductVersion',
    defaultLabel: 'Product Version',
    canonicalName: 'Product Version',
    minWidth: 96,
    defaultWidth: 126,
    maxWidth: 260,
    isFlex: false,
    align: 'left',
    isMono: true,
    requiresBackendProp: true,
    defaultSortDir: 'desc',
    sortKind: 'text',
  },
  {
    id: 'productName',
    tier: 2,
    category: 'program',
    labelKey: 'search.colProductName',
    defaultLabel: 'Product Name',
    canonicalName: 'Product Name',
    minWidth: 110,
    defaultWidth: 170,
    maxWidth: 380,
    isFlex: false,
    align: 'left',
    requiresBackendProp: true,
    defaultSortDir: 'asc',
    sortKind: 'text',
  },
  {
    id: 'company',
    tier: 2,
    category: 'program',
    labelKey: 'search.colCompany',
    defaultLabel: 'Company',
    canonicalName: 'Company',
    minWidth: 100,
    defaultWidth: 160,
    maxWidth: 360,
    isFlex: false,
    align: 'left',
    requiresBackendProp: true,
    defaultSortDir: 'asc',
    sortKind: 'text',
  },
  {
    id: 'fileDescription',
    tier: 2,
    category: 'program',
    labelKey: 'search.colFileDescription',
    defaultLabel: 'File Description',
    canonicalName: 'File Description',
    minWidth: 120,
    defaultWidth: 210,
    maxWidth: 460,
    isFlex: false,
    align: 'left',
    requiresBackendProp: true,
    defaultSortDir: 'asc',
    sortKind: 'text',
  },
  {
    id: 'copyright',
    tier: 2,
    category: 'program',
    labelKey: 'search.colCopyright',
    defaultLabel: 'Copyright',
    canonicalName: 'Copyright',
    minWidth: 120,
    defaultWidth: 200,
    maxWidth: 460,
    isFlex: false,
    align: 'left',
    requiresBackendProp: true,
    defaultSortDir: 'asc',
    sortKind: 'text',
  },

  // ── Tier 2: 校验与哈希 ──────────────────────────────────────────────────────
  {
    id: 'crc32',
    tier: 2,
    category: 'hash',
    labelKey: 'search.colCrc32',
    defaultLabel: 'CRC-32',
    canonicalName: 'CRC-32',
    minWidth: 92,
    defaultWidth: 108,
    maxWidth: 180,
    isFlex: false,
    align: 'left',
    isMono: true,
    requiresBackendProp: true,
    defaultSortDir: 'asc',
    sortKind: 'text',
  },
  {
    id: 'md5',
    tier: 2,
    category: 'hash',
    labelKey: 'search.colMd5',
    defaultLabel: 'MD5',
    canonicalName: 'MD5',
    minWidth: 140,
    defaultWidth: 256,
    maxWidth: 360,
    isFlex: false,
    align: 'left',
    isMono: true,
    requiresBackendProp: true,
    defaultSortDir: 'asc',
    sortKind: 'text',
  },
  {
    id: 'sha256',
    tier: 2,
    category: 'hash',
    labelKey: 'search.colSha256',
    defaultLabel: 'SHA-256',
    canonicalName: 'SHA-256',
    minWidth: 160,
    defaultWidth: 320,
    maxWidth: 560,
    isFlex: false,
    align: 'left',
    isMono: true,
    requiresBackendProp: true,
    defaultSortDir: 'asc',
    sortKind: 'text',
  },

  // ── Tier 2: Windows Property System 精选规范属性 ────────────────────────────
  {
    id: 'System.Keywords',
    tier: 2,
    category: 'windows',
    labelKey: 'search.colWinKeywords',
    defaultLabel: 'Tags',
    canonicalName: 'System.Keywords',
    minWidth: 100,
    defaultWidth: 160,
    maxWidth: 360,
    isFlex: false,
    align: 'left',
    requiresBackendProp: true,
    defaultSortDir: 'asc',
    sortKind: 'text',
  },
  {
    id: 'System.Photo.FNumber',
    tier: 2,
    category: 'windows',
    labelKey: 'search.colWinFNumber',
    defaultLabel: 'F-stop',
    canonicalName: 'System.Photo.FNumber',
    minWidth: 80,
    defaultWidth: 96,
    maxWidth: 180,
    isFlex: false,
    align: 'right',
    isMono: true,
    requiresBackendProp: true,
    defaultSortDir: 'asc',
    sortKind: 'number',
  },
  {
    id: 'System.Photo.ExposureTime',
    tier: 2,
    category: 'windows',
    labelKey: 'search.colWinExposureTime',
    defaultLabel: 'Exposure Time',
    canonicalName: 'System.Photo.ExposureTime',
    minWidth: 92,
    defaultWidth: 112,
    maxWidth: 200,
    isFlex: false,
    align: 'right',
    isMono: true,
    requiresBackendProp: true,
    defaultSortDir: 'asc',
    sortKind: 'number',
  },
  {
    id: 'System.Video.TotalBitrate',
    tier: 2,
    category: 'windows',
    labelKey: 'search.colWinTotalBitrate',
    defaultLabel: 'Total Bitrate',
    canonicalName: 'System.Video.TotalBitrate',
    minWidth: 96,
    defaultWidth: 120,
    maxWidth: 220,
    isFlex: false,
    align: 'right',
    isMono: true,
    requiresBackendProp: true,
    defaultSortDir: 'desc',
    sortKind: 'number',
  },
  {
    id: 'System.Music.TrackNumber',
    tier: 2,
    category: 'windows',
    labelKey: 'search.colWinTrackNumber',
    defaultLabel: 'Track Number',
    canonicalName: 'System.Music.TrackNumber',
    minWidth: 74,
    defaultWidth: 90,
    maxWidth: 160,
    isFlex: false,
    align: 'right',
    isMono: true,
    requiresBackendProp: true,
    defaultSortDir: 'asc',
    sortKind: 'number',
  },
  {
    id: 'System.Media.Year',
    tier: 2,
    category: 'windows',
    labelKey: 'search.colWinMediaYear',
    defaultLabel: 'Year',
    canonicalName: 'System.Media.Year',
    minWidth: 70,
    defaultWidth: 84,
    maxWidth: 160,
    isFlex: false,
    align: 'right',
    isMono: true,
    requiresBackendProp: true,
    defaultSortDir: 'desc',
    sortKind: 'number',
  },
  {
    id: 'System.Media.Publisher',
    tier: 2,
    category: 'windows',
    labelKey: 'search.colWinPublisher',
    defaultLabel: 'Publisher',
    canonicalName: 'System.Media.Publisher',
    minWidth: 100,
    defaultWidth: 150,
    maxWidth: 340,
    isFlex: false,
    align: 'left',
    requiresBackendProp: true,
    defaultSortDir: 'asc',
    sortKind: 'text',
  },
  {
    id: 'System.Document.RevisionNumber',
    tier: 2,
    category: 'windows',
    labelKey: 'search.colWinRevision',
    defaultLabel: 'Revision Number',
    canonicalName: 'System.Document.RevisionNumber',
    minWidth: 88,
    defaultWidth: 112,
    maxWidth: 200,
    isFlex: false,
    align: 'left',
    isMono: true,
    requiresBackendProp: true,
    defaultSortDir: 'asc',
    sortKind: 'text',
  },
  {
    id: 'System.OfflineAvailability',
    tier: 2,
    category: 'windows',
    labelKey: 'search.colWinOfflineAvailability',
    defaultLabel: 'Offline Availability',
    canonicalName: 'System.OfflineAvailability',
    minWidth: 112,
    defaultWidth: 148,
    maxWidth: 280,
    isFlex: false,
    align: 'left',
    requiresBackendProp: true,
    defaultSortDir: 'asc',
    sortKind: 'text',
  },
];

const BUILTIN_COLUMN_MAP = new Map<TableColumnId, TableColumnDefinition>(
  BUILTIN_TABLE_COLUMNS.map((col) => [col.id, col]),
);

const CUSTOM_SYSTEM_PROP_REGEX = /^System\.[A-Za-z][A-Za-z0-9_]*(\.[A-Za-z][A-Za-z0-9_]*)*$/;

export function isValidCustomWindowsPropertyId(id: string): boolean {
  return CUSTOM_SYSTEM_PROP_REGEX.test(id.trim());
}

export function isKnownTableColumnId(id: string): boolean {
  return BUILTIN_COLUMN_MAP.has(id) || isValidCustomWindowsPropertyId(id);
}

/**
 * 判断某列是否可能触发后端按需属性提取：
 * - `requiresBackendProp === true` 的全部扩展列；
 * - `accessed` 与 `attributes` 当原生检索结果未携带有效时间戳/属性位掩码时也允许按需向后端补充提取。
 */
export function isBackendExtractableColumn(colId: TableColumnId): boolean {
  if (colId === 'accessed' || colId === 'attributes') return true;
  return resolveColumnDefinition(colId).requiresBackendProp;
}

/**
 * 判断特定结果行在指定列上是否需要请求后端提取元数据。
 */
export function doesItemRequireBackendProperty(
  item: SearchResult,
  colId: TableColumnId,
): boolean {
  if (colId === 'accessed') {
    return !(typeof item.lastAccessTime === 'number' && Number.isFinite(item.lastAccessTime) && item.lastAccessTime > 0);
  }
  if (colId === 'attributes') {
    return !(typeof item.attributes === 'number' && Number.isFinite(item.attributes) && item.attributes !== 0);
  }
  return resolveColumnDefinition(colId).requiresBackendProp;
}

/**
 * 解析任意列 ID 的完整定义：
 * - 内置列直接 O(1) 命中注册表；
 * - 用户动态添加的 `System.Xxx.Yyy` Windows 属性自动生成强类型定义，无缝接入表头、调宽、拖拽与排序。
 */
export function resolveColumnDefinition(colId: TableColumnId): TableColumnDefinition {
  const builtin = BUILTIN_COLUMN_MAP.get(colId);
  if (builtin) return builtin;

  const shortName = colId.startsWith('System.')
    ? colId.slice(colId.lastIndexOf('.') + 1)
    : colId;

  return {
    id: colId,
    tier: 2,
    category: 'windows',
    labelKey: '',
    defaultLabel: shortName || colId,
    canonicalName: colId,
    minWidth: 96,
    defaultWidth: 156,
    maxWidth: 420,
    isFlex: false,
    align: 'left',
    requiresBackendProp: true,
    defaultSortDir: 'asc',
    sortKind: 'text',
  };
}

export function getColumnDisplayLabel(
  colId: TableColumnId,
  t: (key: string, defaultValue?: string) => string,
): string {
  const def = resolveColumnDefinition(colId);
  if (!def.labelKey) {
    return def.defaultLabel;
  }
  return t(def.labelKey, def.defaultLabel);
}

export function formatFileAttributesBitmask(attributes?: number): string {
  if (!attributes) return '';
  let out = '';
  if (attributes & 0x0001) out += 'R'; // READONLY
  if (attributes & 0x0002) out += 'H'; // HIDDEN
  if (attributes & 0x0004) out += 'S'; // SYSTEM
  if (attributes & 0x0010) out += 'D'; // DIRECTORY
  if (attributes & 0x0020) out += 'A'; // ARCHIVE
  if (attributes & 0x0400) out += 'L'; // REPARSE_POINT
  if (attributes & 0x0800) out += 'C'; // COMPRESSED
  if (attributes & 0x4000) out += 'E'; // ENCRYPTED
  if (attributes & 0x1000) out += 'O'; // OFFLINE
  if (attributes & 0x0100) out += 'T'; // TEMPORARY
  return out;
}

export function extractFileExtension(name: string, isDirectory: boolean): string {
  if (isDirectory || !name) return '';
  const dot = name.lastIndexOf('.');
  if (dot <= 0 || dot === name.length - 1) return '';
  return name.slice(dot + 1).toLowerCase();
}

export function extractFileStem(name: string, isDirectory: boolean): string {
  if (!name) return '';
  if (isDirectory) return name;
  const dot = name.lastIndexOf('.');
  return dot > 0 ? name.slice(0, dot) : name;
}

export function extractDriveLabel(fullPath: string): string {
  if (!fullPath || fullPath.length < 2 || fullPath[1] !== ':') return '';
  return `${fullPath[0].toUpperCase()}:`;
}

export function extractImmediateParentName(fullPath: string): string {
  if (!fullPath) return '';
  const clean = fullPath.replace(/[/\\]+$/, '');
  const lastSlash = Math.max(clean.lastIndexOf('/'), clean.lastIndexOf('\\'));
  if (lastSlash <= 0) return '';
  const parentDir = clean.slice(0, lastSlash);
  if (/^[a-zA-Z]:$/.test(parentDir)) return `${parentDir}\\`;
  const prevSlash = Math.max(parentDir.lastIndexOf('/'), parentDir.lastIndexOf('\\'));
  return prevSlash >= 0 ? parentDir.slice(prevSlash + 1) : parentDir;
}

/**
 * 解析任意单元格在表格视图与 CSV 导出中应显示的文本值（统一前端本地直算与后端惰性提取结果）。
 */
export function resolveTableCellText(
  colId: TableColumnId,
  result: SearchResult,
  propRow: Record<string, PropertyCellValue> | undefined,
  formatters: {
    formatFileSize: (bytes?: number, isDirectory?: boolean) => string;
    formatWindowsTime: (fileTime?: number | string) => string;
    extractParentDirectoryPath: (fullPath: string) => string;
  },
): string {
  switch (colId) {
    case 'name':
      return result.name;
    case 'path':
      return formatters.extractParentDirectoryPath(result.path) || result.path;
    case 'size':
      return formatters.formatFileSize(result.size, result.isDirectory);
    case 'ext':
      return extractFileExtension(result.name, result.isDirectory);
    case 'modified':
      return formatters.formatWindowsTime(result.lastWriteTime);
    case 'created':
      return formatters.formatWindowsTime(result.creationTime);
    case 'accessed':
      if (result.lastAccessTime) {
        return formatters.formatWindowsTime(result.lastAccessTime);
      }
      return propRow?.accessed?.d ?? '';
    case 'attributes':
      if (result.attributes !== undefined && result.attributes !== 0) {
        return formatFileAttributesBitmask(result.attributes);
      }
      return propRow?.attributes?.d ?? (result.isDirectory ? 'D' : '');
    case 'runCount':
      return result.runCount && result.runCount > 0 ? String(result.runCount) : '';
    case 'stem':
      return extractFileStem(result.name, result.isDirectory);
    case 'parentName':
      return extractImmediateParentName(result.path);
    case 'drive':
      return extractDriveLabel(result.path);
    case 'fullPathLength':
      return result.path ? String(result.path.length) : '';
    case 'nameLength':
      return result.name ? String(result.name.length) : '';
    default:
      return propRow?.[colId]?.d ?? '';
  }
}
