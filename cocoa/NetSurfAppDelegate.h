/*
 * Copyright 2011 Sven Weidauer <sven.weidauer@gmail.com>
 *
 * This file is part of NetSurf, http://www.netsurf-browser.org/
 *
 * NetSurf is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License.
 *
 * NetSurf is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#import <Cocoa/Cocoa.h>

@class SearchWindowController;
@class PreferencesWindowController;

@interface NetSurfAppDelegate : NSObject {
	NSWindow *historyWindow;
	SearchWindowController *search;
	PreferencesWindowController *preferences;
}

@property (readwrite, retain, nonatomic) IBOutlet NSWindow *historyWindow;
@property (readwrite, retain, nonatomic) SearchWindowController *search;
@property (readwrite, retain, nonatomic) PreferencesWindowController *preferences;

- (IBAction) showSearchWindow: (id) sender;
- (IBAction) searchForward: (id) sender;
- (IBAction) searchBackward: (id) sender;

- (IBAction) showPreferences: (id) sender;

@end
