;;; host-win.el --- frames that a host application draws -*- lexical-binding: t -*-

;; Copyright (C) 2026 Free Software Foundation, Inc.

;; This file is part of GNU Emacs.

;; GNU Emacs is free software: you can redistribute it and/or modify
;; it under the terms of the GNU General Public License as published by
;; the Free Software Foundation, either version 3 of the License, or
;; (at your option) any later version.

;; GNU Emacs is distributed in the hope that it will be useful,
;; but WITHOUT ANY WARRANTY; without even the implied warranty of
;; MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
;; GNU General Public License for more details.

;; You should have received a copy of the GNU General Public License
;; along with GNU Emacs.  If not, see <https://www.gnu.org/licenses/>.

;;; Commentary:

;; The Lisp side of the `host' window system, whose frames have no
;; window and are drawn by a host application (libemacs/src/hostterm.c).
;; This is what startup.el and frame.el ask of any window system: to be
;; set up, to handle the command line, and to make frames.  Loaded by
;; loadup.el in a build that has it.

;;; Code:

;; This file is loaded as source when Emacs is dumped, because the
;; build compiles only lisp/.  Expanding `cl-defmethod' with a
;; `&context' then needs cl-lib, which may not be loaded while dumping,
;; so the methods below are defined with what `cl-defmethod' expands
;; to, `cl-generic-define-method', as a compiled file would have it.

(unless (featurep 'host)
  (error "%s: Loading host-win without the host window system"
         invocation-name))

;; Documentation-purposes only: actually loaded in loadup.el.
(require 'frame)
(require 'fontset)

;; Every display name is the host's: there is only the one.
(add-to-list 'display-format-alist '(".*" . host))

(defvar x-command-line-resources)

(defvar host-initialized nil
  "Whether the host's display has been opened.")

(cl-generic-define-method
 #'window-system-initialization nil
 '(&context (window-system host) &optional display) nil
 (lambda (&optional display)
   "Open the host's display.  DISPLAY is its name, which does not matter."
   (when host-initialized
     (error "The host's display is already open"))
   (create-default-fontset)
   (x-open-connection (or display "host") x-command-line-resources t)
   (setq host-initialized t)))

(cl-generic-define-method
 #'frame-creation-function nil
 '(params &context (window-system host)) nil
 (lambda (params)
   (x-create-frame-with-faces params)))

(cl-generic-define-method
 #'handle-args-function nil
 '(args &context (window-system host)) nil
 (lambda (args)
   (x-handle-args args)))

;; TODO: the clipboard, as `gui-backend-get-selection' and the other
;; methods select.el defines; their defaults, which do nothing, are
;; what a host frame has until then.

(provide 'host-win)
(provide 'term/host-win)

;;; host-win.el ends here
