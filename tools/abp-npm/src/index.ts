// Copyright 2026 Han Wang. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

export { ABPClient } from "./client.js";
export { launch, findAvailablePort, DEFAULT_START_PORT } from "./launch.js";
export { getExecutablePath } from "./paths.js";
export { ABP_VERSION } from "./paths.js";
export type { LaunchOptions, Browser } from "./launch.js";
export type {
  // Common
  WaitUntil,
  ScreenshotOptions,
  ActionRequest,
  ActionResponse,
  ScreenshotData,
  ScrollPosition,
  ActionTiming,
  ActionEvent,
  ErrorResponse,
  // Browser
  BrowserStatus,
  SessionData,
  ShutdownOptions,
  // Tabs
  Tab,
  CreateTabOptions,
  CreatedTab,
  ActivateResult,
  // Navigation
  Modifier,
  NavigateOptions,
  // Mouse
  ClickOptions,
  MoveOptions,
  ScrollOptions,
  DragOptions,
  // Keyboard
  TypeOptions,
  KeyOptions,
  // Content
  ExecuteOptions,
  ExecuteResult,
  TextOptions,
  TextResult,
  // Wait
  WaitOptions,
  // Dialogs
  DialogInfo,
  AcceptDialogOptions,
  // Execution Control
  ExecutionState,
  SetExecutionOptions,
  // Downloads
  Download,
  ListDownloadsOptions,
  DownloadContentOptions,
  // File Chooser
  FileChooserOptions,
  FileChooserOpenOptions,
  FileChooserSaveOptions,
  FileChooserCancelOptions,
  // Slider
  SliderOptions,
  SliderBaseOptions,
  HorizontalSliderOptions,
  VerticalSliderOptions,
  // Clear Text
  ClearTextOptions,
  // Batch
  BatchAction,
  BatchOptions,
  // Wait for Network
  WaitForNetworkOptions,
  // Permissions
  PermissionRequest,
  GrantPermissionOptions,
  DenyPermissionOptions,
  // Select Popup
  SelectPopupOptions,
  // History
  Session,
  HistoryAction,
  HistoryEvent,
  // Network
  NetworkRequest,
  NetworkQueryOptions,
  NetworkSaveOptions,
  NetworkSaveResult,
  // Console
  ConsoleEntry,
  ConsoleQueryOptions,
  ConsoleQueryResult,
  ConsoleClearOptions,
  // Curl
  CurlOptions,
  CurlResult,
  // Input Mode
  InputMode,
  InputModeResult,
  SetInputModeOptions,
  // CDP Mode
  CdpModeEnterOptions,
  CdpModeEnterResult,
  CdpModeExitResult,
} from "./types.js";
