#include "arkit_setup.h"
#import <UIKit/UIWindow.h>
#import <ARKit/ARKit.h>

@interface RectView : UIView @end
@implementation RectView {
		@public
		uint32_t rects_len;
		CGRect rects[0x400];
	}
	- (void)drawRect:(CGRect)rect {
		const CGContextRef context = UIGraphicsGetCurrentContext();
		CGContextSetFillColor(context, (const CGFloat[4]){1, 1, 0, 1});
		CGContextFillRects(context, self->rects, self->rects_len);
	}
@end

@interface CameraWindow : UIWindow<ARSessionDelegate> @end
@implementation CameraWindow {
		@public
		ARSession *session;
		RectView *markerView;
		UIButton *finishButton;
		void (*onFinish)(void);
	}
	- (void)session:(ARSession*)session didUpdateFrame:(ARFrame*)frame {
		ARCamera *const camera = frame.camera;
		ARPointCloud *const cloud = frame.rawFeaturePoints;
		NSUInteger count = cloud.count;
		const simd_float3 *const points = cloud.points;
		if(count > sizeof(self->markerView->rects) / sizeof(*self->markerView->rects))
			count = sizeof(self->markerView->rects) / sizeof(*self->markerView->rects);
		const UIInterfaceOrientation orientation = self.windowScene.interfaceOrientation;
		const CGSize viewport = self->markerView.bounds.size;
		for(uint32_t i = 0; i < count; ++i) {
			const CGPoint point = [camera projectPoint:points[i] orientation:orientation viewportSize:viewport];
			self->markerView->rects[i] = (CGRect){{point.x - 1, point.y - 1}, {2, 2}};
		}
		self->markerView->rects_len = count;

		self.layer.contents = (__bridge id)frame.capturedImage;
		[self->markerView setNeedsDisplay];
		self->finishButton.hidden = (frame.worldMappingStatus != ARWorldMappingStatusMapped);
	}
	- (void)errorAlert:(NSError*)error title:(NSString*)title {
		UIAlertController *const alert = [UIAlertController alertControllerWithTitle:@"Failed to get world map"
			message:(error != nil ? error.localizedDescription : @"") preferredStyle:UIAlertControllerStyleAlert];
		[alert addAction:[UIAlertAction actionWithTitle:@"OK" style:UIAlertActionStyleDefault handler:nil]];
		[self.rootViewController presentViewController:alert animated:YES completion:nil];
	}
	- (void)tryFinish:(UIButton*)button {
		if(self->session == nil)
			return;
		[self->session getCurrentWorldMapWithCompletionHandler:^(ARWorldMap *const worldMap, NSError *error) {
			if(worldMap == nil || error != nil) {
				[self errorAlert:error title:@"Failed to get world map"];
				return;
			}
			do {
				NSData *const data = [NSKeyedArchiver archivedDataWithRootObject:worldMap requiringSecureCoding:YES error:&error];
				if(data == nil)
					break;
				NSURL *const url = [NSFileManager.defaultManager URLForDirectory:NSDocumentDirectory inDomain:NSUserDomainMask appropriateForURL:nil create:YES error:&error];
				if(url == nil || ![data writeToURL:[url URLByAppendingPathComponent:@"map.arexperience"] options:NSDataWritingAtomic error:&error])
					break;

				[self->session pause];
				self->session.delegate = nil;
				self->session = nil;
				self.hidden = YES;
				self.windowScene = nil;
				void (*const finish)(void) = self->onFinish;
				if(finish == NULL)
					return;
				self->onFinish = NULL;
				finish();
				return;
			} while(false);
			[self errorAlert:error title:@"Failed to save world map"];
		}];
	}
@end

void StartARKitCalibration(void (*const onFinish)(void)) {
	CameraWindow *window = [CameraWindow.alloc initWithFrame:UIScreen.mainScreen.bounds];
	window->onFinish = onFinish;
	window.contentScaleFactor = UIScreen.mainScreen.nativeScale;
	window.opaque = YES;
	window.rootViewController = [UIViewController new];

	window->markerView = [RectView.alloc initWithFrame:UIScreen.mainScreen.bounds];
	window->markerView.opaque = NO;
	window->markerView.userInteractionEnabled = NO;
	[window addSubview:window->markerView];

	window->finishButton = [UIButton buttonWithType:UIButtonTypeSystem];
	[window->finishButton addTarget:window action:@selector(tryFinish:) forControlEvents:UIControlEventTouchUpInside];
	window->finishButton.frame = CGRectMake(20, 20, 80, 40);
	[window->finishButton setTitle:@"Finish" forState:UIControlStateNormal];
	[window->finishButton setTitleColor:UIColor.whiteColor forState:UIControlStateNormal];
	window->finishButton.backgroundColor = window->finishButton.tintColor;
	window->finishButton.layer.cornerRadius = 8;
	window->finishButton.exclusiveTouch = YES;
	window->finishButton.hidden = YES;
	[window.rootViewController.view addSubview:window->finishButton];

	window->session = [ARSession new];
	window->session.delegate = window;
	ARWorldTrackingConfiguration *const config = [ARWorldTrackingConfiguration new];
	config.planeDetection = YES;
	config.worldAlignment = ARWorldAlignmentGravity;
	config.lightEstimationEnabled = NO;
	bool found = false;
	for(ARVideoFormat *const format in ARWorldTrackingConfiguration.supportedVideoFormats) {
		if(format.captureDevicePosition == AVCaptureDevicePositionFront)
			continue;
		if(found && (format.framesPerSecond < config.videoFormat.framesPerSecond || (double)format.imageResolution.width * (double)format.imageResolution.height >=
				(double)config.videoFormat.imageResolution.width * (double)config.videoFormat.imageResolution.height))
			continue;
		config.videoFormat = format;
		found = true;
	}
	[window->session runWithConfiguration:config];

	[window makeKeyAndVisible];
}
