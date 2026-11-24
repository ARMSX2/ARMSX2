// SPDX-License-Identifier: GPL-3.0+

import Foundation
import SwiftUI
import UniformTypeIdentifiers
import UIKit

struct ImportDocumentPicker: UIViewControllerRepresentable {
	let allowedContentTypes: [UTType]
	let allowsMultipleSelection: Bool
	let asCopy: Bool
	let onComplete: (Result<[URL], Error>) -> Void

	init(
		allowedContentTypes: [UTType] = [],
		allowsMultipleSelection: Bool,
		asCopy: Bool = true,
		onComplete: @escaping (Result<[URL], Error>) -> Void
	) {
		self.allowedContentTypes = allowedContentTypes
		self.allowsMultipleSelection = allowsMultipleSelection
		self.asCopy = asCopy
		self.onComplete = onComplete
	}

    func makeCoordinator() -> Coordinator {
        Coordinator(onComplete: onComplete)
    }

    func makeUIViewController(context: Context) -> UIDocumentPickerViewController {
		let picker = UIDocumentPickerViewController(forOpeningContentTypes: allowedContentTypes, asCopy: asCopy)
        picker.allowsMultipleSelection = allowsMultipleSelection
        picker.delegate = context.coordinator
        return picker
    }

    func updateUIViewController(_ uiViewController: UIDocumentPickerViewController, context: Context) {}

    final class Coordinator: NSObject, UIDocumentPickerDelegate {
        private let onComplete: (Result<[URL], Error>) -> Void

        init(onComplete: @escaping (Result<[URL], Error>) -> Void) {
            self.onComplete = onComplete
        }

        func documentPicker(_ controller: UIDocumentPickerViewController, didPickDocumentsAt urls: [URL]) {
            onComplete(.success(urls))
        }

        func documentPickerWasCancelled(_ controller: UIDocumentPickerViewController) {
            onComplete(.failure(CocoaError(.userCancelled)))
        }
    }
}
